#include "ycash_rpc_client.h"
#include "Logger.h"
#include "services/RpcResponseCache.h"
#include <cstddef>
#include <format>
#include <optional>
#include <unordered_map>
#include <curl/curl.h>
#include "glaze/glaze.hpp" // IWYU pragma: keep
#include "models/RpcResponses.h"


namespace {

// Which methods may be served from RpcResponseCache, and under which
// invalidation class. Anything not listed here always goes to the node:
// getblockcount/getbestblockhash are freshness probes, and the periodic
// state snapshot (getblockchaininfo, getrawmempool, ...) is already cached
// in BlockchainState.
std::optional<RpcResponseCache::Class> cacheable_class(const std::string& method)
{
    using Class = RpcResponseCache::Class;

    // Pure function of height — the answer can never change.
    if (method == "getblocksubsidy")
        return Class::Immutable;

    // These answers (including their confirmations / nextblockhash fields)
    // only change when a new block arrives.
    if (method == "getblock" || method == "getblockhash" ||
        method == "getrawtransaction" || method == "getaddressbalance" ||
        method == "getaddressdeltas" || method == "getaddressfirstlastheight")
        return Class::BlockDependent;

    // Per-address mempool view changes with every mempool update.
    if (method == "getaddressmempool")
        return Class::MempoolDependent;

    return std::nullopt;
}

// Cache key = method + serialized params. Empty on serialization failure,
// which callers treat as "don't cache".
std::string make_rpc_cache_key(const std::string& method, const glz::generic& params)
{
    std::string params_json;
    if (glz::write_json(params, params_json))
        return "";
    return method + "|" + params_json;
}

} // namespace


// CURL callback for writing response to std::string
static size_t curl_write_cb(void* contents, size_t size, size_t nmemb, std::string* userp)
{
    userp->append((char*)contents, size * nmemb);
    return size * nmemb;
}

// CurlHandle implementation
CurlHandle::CurlHandle() : curl_handle(curl_easy_init()) {

}

CurlHandle::~CurlHandle() {
    if (curl_handle != nullptr) {
        curl_easy_cleanup(curl_handle);
    }
}


YcashRpcClient::YcashRpcClient(const std::string& host, int port,
                               const std::string& username, const std::string& password)
    : rpc_host(host), rpc_port(port), rpc_user(username), rpc_pass(password) {
    rpc_config = RpcConfig::get_default();
    rpc_config.host = host;
    rpc_config.port = port;
    rpc_config.username = username;
    rpc_config.password = password;

    // Initialize dynamic batch sizing state
    configured_batch_size = rpc_config.block_headers_batch_size;
    recommended_batch_size = configured_batch_size;

    // Initialize reusable CURL handles for persistent connections
    // CurlHandle constructor automatically calls curl_easy_init()
    if (curl_main) {
        initialize_persistent_connection(curl_main.get());
    }

    if (curl_adhoc) {
        initialize_persistent_connection(curl_adhoc.get());
    }
}

YcashRpcClient::YcashRpcClient(const RpcConfig::Config& config)
    : rpc_host(config.host), rpc_port(config.port),
      rpc_user(config.username), rpc_pass(config.password),
      rpc_config(config) {

    // Initialize dynamic batch sizing state
    configured_batch_size = rpc_config.block_headers_batch_size;
    recommended_batch_size = configured_batch_size;

    // Initialize reusable CURL handles for persistent connections
    // CurlHandle constructor automatically calls curl_easy_init()
    if (curl_main) {
        initialize_persistent_connection(curl_main.get());
    }

    if (curl_adhoc) {
        initialize_persistent_connection(curl_adhoc.get());
    }
}

YcashRpcClient::~YcashRpcClient() = default;

// Static member definitions
std::unique_ptr<YcashRpcClient> YcashRpcClient::client_instance = nullptr;

YcashRpcClient& YcashRpcClient::instance() {
    if (!client_instance) {
        client_instance = std::make_unique<YcashRpcClient>();
    }
    return *client_instance;
}

YcashRpcClient& YcashRpcClient::thread_instance() {
    thread_local YcashRpcClient client(instance().rpc_config);
    return client;
}

// Get curl handle for the specified backend
CURL* YcashRpcClient::get_curl_handle_for_backend(Backend backend) const
{
    switch (backend)
    {
        case Backend::Main : return curl_main.get();
        case Backend::Adhoc : return curl_adhoc.get();
        case Backend::State : return curl_state.get();
    }

    return nullptr;
}

bool YcashRpcClient::initialize(const RpcConfig::Config& config) {
    // Initialize the singleton instance with both curl backends
    YcashRpcClient& client = instance();

    client.rpc_host = config.host;
    client.rpc_port = config.port;
    client.rpc_user = config.username;
    client.rpc_pass = config.password;
    client.rpc_config = config;
    client.configured_batch_size = config.block_headers_batch_size;
    client.recommended_batch_size = config.block_headers_batch_size;

    // Initialize all curl handles with connection options
    if (client.curl_main) {
        client.initialize_persistent_connection(client.curl_main.get());
    }

    if (client.curl_adhoc) {
        client.initialize_persistent_connection(client.curl_adhoc.get());
    }

    if (client.curl_state) {
        client.initialize_persistent_connection(client.curl_state.get());
    }

    return true;
}

int YcashRpcClient::get_recommended_batch_size() const {
    std::scoped_lock<std::mutex> lock(batch_state_mutex);
    return recommended_batch_size;
}



void YcashRpcClient::initialize_persistent_connection(CURL* handle) const
{
    // Enable TCP keep-alive for persistent connections
    curl_easy_setopt(handle, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(handle, CURLOPT_TCP_KEEPIDLE, 120L);
    curl_easy_setopt(handle, CURLOPT_TCP_KEEPINTVL, 60L);

    // Connection reuse settings
    curl_easy_setopt(handle, CURLOPT_FORBID_REUSE, 0L);  // Allow connection reuse
    curl_easy_setopt(handle, CURLOPT_FRESH_CONNECT, 0L);  // Reuse existing connections

    // Build RPC URL
    std::string rpc_url = std::format("http://{}:{}/", rpc_host, rpc_port);

    curl_easy_setopt(handle, CURLOPT_URL, rpc_url.c_str());
    curl_easy_setopt(handle, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
    curl_easy_setopt(handle, CURLOPT_USERPWD, (rpc_user + ":" + rpc_pass).c_str());
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, (long)rpc_config.connection_timeout);  // Connection timeout (5s)
    curl_easy_setopt(handle, CURLOPT_TIMEOUT, (long)rpc_config.timeout);  // Total timeout (10s)
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(handle, CURLOPT_BUFFERSIZE, 1024 * 1024);

}

bool YcashRpcClient::make_json_rpc_request(RpcRequest request, std::string &response_string) const
{
    Logger& logger = Logger::instance();
    std::string request_string;
    auto ec = glz::write_json(request, request_string);

    if (ec) {
        logger.errorf("Error writing JSON: {}", ec.custom_error_message);
        response_string = "";
        return false;
    }

    // Check if CURL handle is initialized
    if (!curl_main) {
        Logger& logger = Logger::instance();
        logger.error("RPC: CURL handle not initialized");
        response_string = "Failed to initialize HTTP client";
        return false;
    }

    // Set request-specific options (constant options are set in initialize_persistent_connection)
    curl_easy_setopt(curl_main.get(), CURLOPT_POSTFIELDS, request_string.c_str());
    curl_easy_setopt(curl_main.get(), CURLOPT_POSTFIELDSIZE, (long)request_string.size());
    curl_easy_setopt(curl_main.get(), CURLOPT_WRITEDATA, &response_string);

    // Set headers for this request
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "User-Agent: Inzyght/1.0");
    curl_easy_setopt(curl_main.get(), CURLOPT_HTTPHEADER, headers);

    // Make the request
    CURLcode res = curl_easy_perform(curl_main.get());

    if (res != CURLE_OK) {
        auto err = std::string(curl_easy_strerror(res));
        logger.errorf("RPC connection failed: {}", err);
        response_string = std::format("RPC connection failed: {}", err);
        curl_slist_free_all(headers);
        return false;
    }

    curl_slist_free_all(headers);

    return true;

}

bool YcashRpcClient::make_multi_json_rpc_request(const std::vector<RpcRequest> &multirequest, std::string &response_string,
                                                   Backend backend) const
{
    Logger& logger = Logger::instance();
    std::string request_string;
    auto ec = glz::write_json(multirequest, request_string);

    if (ec) {
        logger.errorf("Error writing JSON: {}", ec.custom_error_message);
        response_string = "";
        return false;
    }

    CURL* handle = get_curl_handle_for_backend(backend);
    if (!handle) {
        logger.error("RPC: CURL handle not initialized");
        response_string = "Failed to initialize HTTP client";
        return false;
    }

    // Set request-specific options (constant options are set in initialize_persistent_connection)
    curl_easy_setopt(handle, CURLOPT_POSTFIELDS, request_string.c_str());
    curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, (long)request_string.size());
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &response_string);

    // Set headers for this request
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "User-Agent: Inzyght/1.0");
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);

    // Make the request
    CURLcode res = curl_easy_perform(handle);

    if (res != CURLE_OK) {
        auto err = std::string(curl_easy_strerror(res));
        logger.errorf("RPC connection failed: {}", err);
        response_string = std::format("RPC connection failed: {}", err);
        curl_slist_free_all(headers);
        return false;
    }

    curl_slist_free_all(headers);

    return true;

}

// Perform the HTTP call and return the raw response body string.
// Handles curl setup, execution, and error reporting only — no JSON parsing.
std::string YcashRpcClient::perform_rpc_http(const std::string& method,
                                              const glz::generic& params,
                                              Backend backend) const
{
    Logger& logger = Logger::instance();

    glz::generic rpc_request = {
        {"jsonrpc", "1.0"},
        {"method", method},
        {"params", params},
        {"id", 1}
    };

    std::string request_body;
    auto ec = glz::write_json(rpc_request, request_body);
    if (ec) {
        logger.errorf("RPC: Failed to serialize request: {}", glz::format_error(ec, request_body));
        throw std::runtime_error("Failed to serialize RPC request");
    }

    CURL* handle = get_curl_handle_for_backend(backend);
    if (!handle)
        throw std::runtime_error("CURL handle not initialized");

    std::string response_body;
    curl_easy_setopt(handle, CURLOPT_POSTFIELDS, request_body.c_str());
    curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, (long)request_body.size());
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &response_body);

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "User-Agent: Inzyght/1.0");
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(handle);
    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        auto err = std::string(curl_easy_strerror(res));
        logger.errorf("RPC connection failed: {}", err);
        throw std::runtime_error(std::format("RPC connection failed: {}", err));
    }

    return response_body;
}

// Extracts the "result" value as a raw JSON substring, avoiding a full
// parse/re-serialize round-trip through glz::generic. Used for endpoints
// that forward raw ycashd JSON to the frontend (block/tx modals), where
// preserving the original field order matters for display.
std::string YcashRpcClient::extract_result_raw(const std::string& rpc_envelope)
{
    auto key_pos = rpc_envelope.find("\"result\"");
    if (key_pos == std::string::npos)
        return "";

    auto colon = rpc_envelope.find(':', key_pos + 8);
    if (colon == std::string::npos)
        return "";

    size_t pos = colon + 1;
    while (pos < rpc_envelope.size() && std::isspace((unsigned char)rpc_envelope[pos]))
        ++pos;

    if (pos >= rpc_envelope.size())
        return "";

    char first = rpc_envelope[pos];

    if (first == '{' || first == '[') {
        char open  = first;
        char close = (first == '{') ? '}' : ']';
        int depth = 0;
        bool in_str = false;
        size_t begin = pos;
        for (; pos < rpc_envelope.size(); ++pos) {
            char c = rpc_envelope[pos];
            if (in_str) {
                if (c == '\\') ++pos;       // skip escaped character
                else if (c == '"') in_str = false;
            } else {
                if      (c == '"')  in_str = true;
                else if (c == open)  ++depth;
                else if (c == close) { if (--depth == 0) return rpc_envelope.substr(begin, pos - begin + 1); }
            }
        }
        return "";
    }

    if (first == '"') {
        size_t begin = pos++;
        for (; pos < rpc_envelope.size(); ++pos) {
            if (rpc_envelope[pos] == '\\') { ++pos; continue; }
            if (rpc_envelope[pos] == '"')  return rpc_envelope.substr(begin, pos - begin + 1);
        }
        return "";
    }

    // number / boolean / null
    size_t begin = pos;
    while (pos < rpc_envelope.size()) {
        char c = rpc_envelope[pos];
        if (c == ',' || c == '}' || c == ']' || std::isspace((unsigned char)c)) break;
        ++pos;
    }
    return rpc_envelope.substr(begin, pos - begin);
}

std::string YcashRpcClient::make_json_rpc_request(const std::string& method,
                                                   const glz::generic& params,
                                                   Backend backend) const {
    // Serve repeat lookups (same method + params) from the event-invalidated
    // cache: between two blocks these answers are byte-identical, so this
    // skips the node round-trip entirely.
    const auto cache_class = cacheable_class(method);
    std::string cache_key;
    if (cache_class) {
        cache_key = make_rpc_cache_key(method, params);
        if (!cache_key.empty()) {
            if (auto cached = RpcResponseCache::instance().get(cache_key)) {
                Logger::instance().debugf("RPC method: {} served from cache", method);
                return *cached;
            }
        }
    }

    try {
        std::string response_body = perform_rpc_http(method, params, backend);

        Logger& logger = Logger::instance();

        // Validate response is valid JSON
        glz::generic response;
        auto parse_ec = glz::read_json(response, response_body);
        if (parse_ec) {
            logger.errorf("RPC: Failed to parse JSON response: {}", glz::format_error(parse_ec, response_body));
            return R"({"error":"Invalid JSON response"})";
        }

        // Check for RPC-level errors in the response
        if (response.contains("error") && !response["error"].is_null()) {
            logger.warnf("RPC Error detected in response for method: {}: {}", method, response["error"].dump().value_or("Nemam pojma"));
            return std::format(R"({{"error":"RPC error"}})");;
        }

        logger.debugf("RPC method: {} completed successfully", method);

        // Extract the "result" field from the JSON-RPC 1.0 response
        // The response has the structure: {"jsonrpc":"1.0","result":<data>,"error":null,"id":1}
        // We return just the <data> part so calling code can parse it directly into typed structs
        if (response.contains("result")) {
            // Serialize the result field back to JSON string for parsing by caller
            std::string result_json;
            auto result_ec = glz::write_json(response["result"], result_json);
            if (result_ec) {
                logger.errorf("Failed to serialize RPC result: {}", glz::format_error(result_ec));
                return R"({"error":"Failed to serialize RPC result"})";
            }
            // Only successful responses are cached — error returns above fall
            // through uncached so transient failures aren't pinned.
            if (cache_class && !cache_key.empty()) {
                RpcResponseCache::instance().put(cache_key, result_json, *cache_class);
            }
            return result_json;
        }

        // If no result field, return error
        logger.errorf("No result field in RPC response: {}", response_body);
        return R"({"error":"No result field in RPC response"})";

    } catch (const std::exception& e) {
        Logger& logger = Logger::instance();
        logger.errorf("RPC Exception: {}", e.what());
        return std::format(R"({{"error":"Exception: {}"}})", e.what());
    }
}



YcashBlock YcashRpcClient::get_block_by_hash(const std::string& hash, int verbosity) const
{
    std::vector<glz::generic> params_vec{glz::generic(hash), glz::generic(verbosity)};
    std::string json_response = make_json_rpc_request("getblock", glz::generic(params_vec));
    YcashBlock block;
    auto ec = glz::read<glz::opts{.error_on_unknown_keys = false}>(block, json_response);
    if (ec) {
        throw std::runtime_error(std::format("Failed to parse block: {}", glz::format_error(ec, json_response)));
    }
    // An RPC error payload ({"error":...}) parses "successfully" into a
    // default-constructed block because unknown keys are ignored — detect it here.
    if (block.hash.empty()) {
        throw std::runtime_error("Block not found");
    }
    return block;
}

std::string YcashRpcClient::get_block_raw_json(const std::string& hash) const
{
    // "raw|" prefix keeps these order-preserving results separate from the
    // re-serialized ones make_json_rpc_request caches for the same params.
    const std::string cache_key = "raw|getblock|" + hash;
    if (auto cached = RpcResponseCache::instance().get(cache_key))
        return *cached;

    std::vector<glz::generic> params_vec{glz::generic(hash), glz::generic(2)};
    std::string envelope = perform_rpc_http("getblock", glz::generic(params_vec), Backend::Main);
    std::string result = extract_result_raw(envelope);
    if (!result.empty())
        RpcResponseCache::instance().put(cache_key, result, RpcResponseCache::Class::BlockDependent);
    return result;
}

YcashBlockVerbose YcashRpcClient::get_block_verbose_by_hash(const std::string& hash) const
{
    std::vector<glz::generic> params_vec{glz::generic(hash), glz::generic(2)};
    std::string json_response = make_json_rpc_request("getblock", glz::generic(params_vec));
    YcashBlockVerbose block;
    auto ec = glz::read<glz::opts{.error_on_unknown_keys = false}>(block, json_response);
    if (ec) {
        throw std::runtime_error(std::format("Failed to parse verbose block: {}", glz::format_error(ec, json_response)));
    }
    if (block.hash.empty()) {
        throw std::runtime_error("Block not found");
    }
    return block;
}

BlockCountResponse YcashRpcClient::get_block_count() const
{
    std::string json_response = make_json_rpc_request("getblockcount", glz::generic::array_t {}, Backend::Adhoc);

    BlockCountResponse response {};

    try
    {
        response.count = std::stoi(json_response);
    }
    catch (const std::exception& e)
    {
        throw std::runtime_error(std::format("Failed to parse block count: {}", e.what()));
    }

    return response;
}

BlockHashResponse YcashRpcClient::get_best_block_hash() const
{
    // Freshness probe (PollingFallback tip detection) — deliberately not in
    // the cacheable set, so this always hits the node.
    std::string json_response = make_json_rpc_request("getbestblockhash", glz::generic::array_t {}, Backend::Adhoc);

    BlockHashResponse response;
    std::string wrapped = std::string("{\"hash\":") + json_response + "}";
    auto ec = glz::read<glz::opts{.error_on_unknown_keys = false}>(response, wrapped);
    if (ec) {
        throw std::runtime_error(std::format("Failed to parse best block hash: {}", glz::format_error(ec, wrapped)));
    }
    return response;
}

BlockHashResponse YcashRpcClient::get_block_hash(int height) const
{
    std::vector<glz::generic> params_vec{glz::generic(height)};
    std::string json_response = make_json_rpc_request("getblockhash", glz::generic(params_vec), Backend::Adhoc);

    BlockHashResponse response;
    std::string wrapped = std::string("{\"hash\":") + json_response + "}";
    auto ec = glz::read<glz::opts{.error_on_unknown_keys = false}>(response, wrapped);
    if (ec) {
        throw std::runtime_error(std::format("Failed to parse block hash: {}", glz::format_error(ec, wrapped)));
    }
    return response;
}


std::string YcashRpcClient::get_transaction_raw_json(const std::string& tx_hash) const
{
    const std::string cache_key = "raw|getrawtransaction|" + tx_hash;
    if (auto cached = RpcResponseCache::instance().get(cache_key))
        return *cached;

    std::vector<glz::generic> params_vec{tx_hash, 1};
    std::string envelope = perform_rpc_http("getrawtransaction", glz::generic(params_vec), Backend::Adhoc);
    std::string result = extract_result_raw(envelope);
    if (!result.empty())
        RpcResponseCache::instance().put(cache_key, result, RpcResponseCache::Class::BlockDependent);
    return result;
}

RawTransaction YcashRpcClient::get_raw_transaction(const std::string& tx_hash, const std::string& block_hash) const
{
    // verbose = 1 to get decoded transaction. When block_hash is supplied it is
    // passed as the 3rd arg so the node can find txs not on the active chain.
    std::vector<glz::generic> params_vec{tx_hash, 1};
    if (!block_hash.empty()) {
        params_vec.emplace_back(block_hash);
    }
    glz::generic params(params_vec);
    std::string json_response = make_json_rpc_request("getrawtransaction", params, Backend::Adhoc);

    RawTransaction tx;
    auto ec = glz::read<glz::opts{.error_on_unknown_keys = false}>(tx, json_response);
    if (ec) {
        throw std::runtime_error(std::format("Failed to parse raw transaction: {}", glz::format_error(ec, json_response)));
    }
    return tx;
}



NetworkInfoResponse YcashRpcClient::get_network_info() const
{
    std::vector<glz::generic> params_vec{}; glz::generic params(params_vec);
    std::string json_response = make_json_rpc_request("getnetworkinfo", params, Backend::Adhoc);

    NetworkInfoResponse info;
    auto ec = glz::read<glz::opts{.error_on_unknown_keys = false}>(info, json_response);
    if (ec) {
        throw std::runtime_error(std::format("Failed to parse network info: {}", glz::format_error(ec, json_response)));
    }
    return info;
}

BlockchainInfoResponse YcashRpcClient::get_chain_info() const
{
    std::vector<glz::generic> params_vec{}; glz::generic params(params_vec);
    std::string json_response = make_json_rpc_request("getblockchaininfo", params, Backend::Adhoc);

    BlockchainInfoResponse info;
    auto ec = glz::read<glz::opts{.error_on_unknown_keys = false}>(info, json_response);
    if (ec) {
        throw std::runtime_error(std::format("Failed to parse blockchain info: {}", glz::format_error(ec, json_response)));
    }
    return info;
}

InfoResponse YcashRpcClient::get_info() const
{
    std::vector<glz::generic> params_vec{};
    glz::generic params(params_vec);
    std::string json_response = make_json_rpc_request("getinfo", glz::generic::array_t{}, Backend::Adhoc);

    InfoResponse info;
    auto ec = glz::read<glz::opts{.error_on_unknown_keys = false}>(info, json_response);
    if (ec) {
        throw std::runtime_error(std::format("Failed to parse info response: {}", glz::format_error(ec, json_response)));
    }
    return info;
}


// Batch get block headers by height (true JSON-RPC batch - single HTTP POST)
std::vector<YcashBlock> YcashRpcClient::batch_get_block_headers(const std::vector<int>& heights, int verbosity) const
{
    std::vector<JsonRpcResponse<YcashBlock>> rpc_results {};
    std::vector<YcashBlock> results {};
    
    if (heights.empty())
    {
        return results;
    }
    
    const size_t number_of_elements {heights.size()};
    std::vector<RpcRequest> multi_rpc_request {};
    multi_rpc_request.reserve(number_of_elements);
    rpc_results.reserve(number_of_elements);
    results.reserve(number_of_elements);

    for (const int& one_height : heights)
    {
        RpcRequest single_rpc_request {};
        single_rpc_request.method = "getblock";
        single_rpc_request.params.emplace_back(std::to_string(one_height));
        single_rpc_request.params.emplace_back(verbosity);
        single_rpc_request.id = one_height; 
        multi_rpc_request.emplace_back(std::move(single_rpc_request));
    }

    std::string response_body;
    make_multi_json_rpc_request(multi_rpc_request, response_body);

    multi_rpc_request.clear();
    multi_rpc_request.shrink_to_fit();

    Logger& logger = Logger::instance();

    if (response_body.empty())
    {
        logger.error("RPC: Empty response from batch request");
        return results;
    }

    auto error = glz::read<glz::opts{.error_on_unknown_keys = false}>(rpc_results, response_body);

    if (error)
    {
        logger.errorf("GLZ Failed to parse JSON: {}",  glz::format_error(error, response_body));
    }

    response_body.clear();
    response_body.shrink_to_fit();

    // JSON-RPC batch responses may arrive in any order, and any element can
    // carry an error instead of a result. Index successful results by id (the
    // height we set on each request) and re-emit in the requested order. Crucially,
    // OMIT any missing/errored entry rather than substituting a default YcashBlock{}:
    // a default block (time=0, empty hash) would otherwise be written to the DB as a
    // zero-timestamp / invalid block, and — because the size check below would still
    // pass — would silently corrupt the batch. Omitting it makes size != heights,
    // which triggers the caller's retry.
    std::unordered_map<int, YcashBlock> by_height;
    by_height.reserve(rpc_results.size());
    for (auto& r : rpc_results)
    {
        if (r.result.has_value())
        {
            by_height.emplace(r.id, std::move(r.result.value()));
        }
        else if (r.error.has_value())
        {
            logger.warnf("RPC: getblock id {} returned error: {}", r.id, r.error.value());
        }
    }

    for (const int h : heights)
    {
        auto it = by_height.find(h);
        if (it != by_height.end())
        {
            results.emplace_back(std::move(it->second));
        }
        // else: response for this height was missing/errored — leave it out so
        // the caller sees a short vector and retries the batch.
    }

    rpc_results.clear();
    rpc_results.shrink_to_fit();

    return results;
}


std::vector<BlockSubsidy> YcashRpcClient::batch_get_block_subsidies(const std::vector<int>& heights) const
{
    std::vector<BlockSubsidy> results;
    std::vector<JsonRpcResponse<BlockSubsidy>> rpc_results;
    
    if (heights.empty()) {
        return results;
    }

    Logger& logger = Logger::instance();

    std::vector<RpcRequest> multi_rpc_request {};
    multi_rpc_request.reserve(heights.size());

    for (const int& one_height : heights)
    {
        if (one_height > 0)
        {
            RpcRequest single_rpc_request {};
            single_rpc_request.method = "getblocksubsidy";
            single_rpc_request.params.emplace_back(one_height);
            single_rpc_request.id = one_height; 
            multi_rpc_request.emplace_back(std::move(single_rpc_request));            
        }
    }

    std::string response_body;
    make_multi_json_rpc_request(multi_rpc_request, response_body);

    if (response_body.empty()) {
        logger.error("RPC: Empty response from batch request");
        return results;
    }

    multi_rpc_request.clear();
    multi_rpc_request.shrink_to_fit();
    
    auto error = glz::read<glz::opts{.error_on_unknown_keys = false}>(rpc_results, std::move(response_body));

    if (error) {
        logger.errorf("GLZ Failed to parse JSON: {}",  glz::format_error(error, response_body));
    }

    response_body.clear();
    response_body.shrink_to_fit();

    for (const auto &one : rpc_results) {
        results.emplace_back(one.result.value_or(BlockSubsidy {}));
    }

    rpc_results.clear();
    rpc_results.shrink_to_fit();

    return results;
}




// Batch get coinbase transactions (true JSON-RPC batch - single HTTP POST)
std::vector<Transaction> YcashRpcClient::batch_get_coinbase_transactions(const std::vector<std::pair<int, std::string>>& cb_txids) const
{
    std::vector<Transaction> results;
    std::vector<JsonRpcResponse<Transaction>> rpc_results;
    
    if (cb_txids.empty()) {
        return results;
    }

    Logger& logger = Logger::instance();

    std::vector<RpcRequest> multi_rpc_request {};
    multi_rpc_request.reserve(cb_txids.size());

    int call_id {1};

    for (const auto& [height, txid] : cb_txids)
    {
        RpcRequest single_rpc_request {};
        single_rpc_request.method = "getrawtransaction";
        single_rpc_request.params.emplace_back(txid);
        single_rpc_request.params.emplace_back(2);
        single_rpc_request.id = call_id++; 
        multi_rpc_request.emplace_back(std::move(single_rpc_request));            
    }

    std::string response_body;
    make_multi_json_rpc_request(multi_rpc_request, response_body);

    multi_rpc_request.clear();
    multi_rpc_request.shrink_to_fit();

    if (response_body.empty())
    {
        logger.error("RPC: Empty response from batch request");
        return results;
    }

    auto error = glz::read<glz::opts{.error_on_unknown_keys = false}>(rpc_results, std::move(response_body));

    if (error)
    {
        logger.errorf("GLZ Failed to parse JSON: {}",  glz::format_error(error, response_body));
    }

    response_body.clear();
    response_body.shrink_to_fit();

    for (const auto& [jsonrpc, id, result, error] : rpc_results)
    {
        results.emplace_back(result.value_or(Transaction {}));
    }

    rpc_results.clear();
    rpc_results.shrink_to_fit();

    return results;
}


// Helper: build {"addresses": [addr]} param object
static glz::generic make_address_param(const std::string& address)
{
    glz::generic addr_list = glz::generic::array_t{};
    addr_list.get_array().emplace_back(address);
    glz::generic obj;
    obj["addresses"] = std::move(addr_list);
    return obj;
}

AddressBalance YcashRpcClient::get_address_balance_single(const std::string& address) const
{
    glz::generic param = make_address_param(address);
    std::vector<glz::generic> params_vec{std::move(param)};
    std::string json = make_json_rpc_request("getaddressbalance", glz::generic(params_vec), Backend::Adhoc);
    AddressBalance balance;
    auto ec = glz::read<glz::opts{.error_on_unknown_keys = false}>(balance, json);
    if (ec)
        throw std::runtime_error(std::format("Failed to parse address balance: {}", glz::format_error(ec, json)));
    return balance;
}

std::vector<AddressDelta> YcashRpcClient::get_address_deltas_single(const std::string& address,
                                                                     int start_height, int end_height) const
{
    glz::generic param = make_address_param(address);
    // Node rejects start=0 / end=0 — only include when explicitly limiting the range
    if (start_height > 0) param["start"] = start_height;
    if (end_height   > 0) param["end"]   = end_height;
    std::vector<glz::generic> params_vec{std::move(param)};
    std::string json = make_json_rpc_request("getaddressdeltas", glz::generic(params_vec), Backend::Adhoc);
    std::vector<AddressDelta> deltas;
    auto ec = glz::read<glz::opts{.error_on_unknown_keys = false}>(deltas, json);
    if (ec)
        throw std::runtime_error(std::format("Failed to parse address deltas: {}", glz::format_error(ec, json)));
    return deltas;
}

std::vector<AddressMempoolTransaction> YcashRpcClient::get_address_mempool_single(const std::string& address) const
{
    glz::generic param = make_address_param(address);
    std::vector<glz::generic> params_vec{std::move(param)};
    std::string json = make_json_rpc_request("getaddressmempool", glz::generic(params_vec), Backend::Adhoc);
    std::vector<AddressMempoolTransaction> mempool;
    auto ec = glz::read<glz::opts{.error_on_unknown_keys = false}>(mempool, json);
    if (ec)
        throw std::runtime_error(std::format("Failed to parse address mempool: {}", glz::format_error(ec, json)));
    return mempool;
}

std::pair<int,int> YcashRpcClient::get_address_first_last_height(const std::string& address) const
{
    std::vector<glz::generic> params_vec{ glz::generic(address) };
    std::string json = make_json_rpc_request("getaddressfirstlastheight", glz::generic(params_vec), Backend::Adhoc);
    glz::generic obj;
    auto ec = glz::read<glz::opts{.error_on_unknown_keys = false}>(obj, json);
    if (ec)
        throw std::runtime_error(std::format("getaddressfirstlastheight parse failed: {} raw={}",
                                             glz::format_error(ec, json), json));
    int fh = obj["firstHeight"].as<int>();
    int lh = obj["lastHeight"].as<int>();
    return {fh, lh};
}











