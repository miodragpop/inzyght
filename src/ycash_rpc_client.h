#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <curl/curl.h>
#include "RpcConfig.h"

#include "models/RpcResponses.h"

// RAII wrapper for CURL* handles
// Automatically manages lifetime: initializes via curl_easy_init()
// and cleans up via curl_easy_cleanup()
class CurlHandle
{
    public:
        // Constructor: Initialize CURL handle via curl_easy_init()
        CurlHandle();

        // Destructor: Clean up CURL handle via curl_easy_cleanup()
        ~CurlHandle();

        // Deleted copy operations (exclusive ownership)
        CurlHandle(const CurlHandle&) = delete;
        CurlHandle& operator=(const CurlHandle&) = delete;

        // Defaulted move operations (enable efficient transfer)
        CurlHandle(CurlHandle&& other) noexcept = default;
        CurlHandle& operator=(CurlHandle&& other) noexcept = default;

        // Access underlying CURL pointer (const and non-const)
        CURL* get() const { return curl_handle; }
        CURL* get() { return curl_handle; }

        // Check if handle is valid
        explicit operator bool() const { return curl_handle != nullptr; }

    private:
        CURL* curl_handle;
};

struct RpcRequest {
    int id;
    std::string method;
    std::vector<std::variant<int, std::string, bool>> params;
};

class YcashRpcClient {
public:
    // Backend selection for curl handles
    enum class Backend { Main, Adhoc, State };

    // Constructor with explicit credentials
    YcashRpcClient(const std::string& host = "127.0.0.1", int port = 8232,
                   const std::string& username = "", const std::string& password = "");

    // Constructor from configuration
    explicit YcashRpcClient(const RpcConfig::Config& config);

    // Singleton access
    static YcashRpcClient& instance();
    static bool initialize(const RpcConfig::Config& config);

    ~YcashRpcClient();



    // Block-related RPCs
    YcashBlock get_block_by_hash(const std::string& hash, int verbosity = 1) const;
    YcashBlockVerbose get_block_verbose_by_hash(const std::string& hash) const;
    std::string get_block_raw_json(const std::string& hash) const;
    BlockCountResponse get_block_count() const;
    BlockHashResponse get_block_hash(int height) const;

    // Batch get block headers and coinbase transactions
    std::vector<YcashBlock> batch_get_block_headers(const std::vector<int>& heights, int verbosity = 1) const;
    std::vector<Transaction> batch_get_coinbase_transactions(const std::vector<std::pair<int, std::string>>& cb_txids) const;
    std::vector<BlockSubsidy> batch_get_block_subsidies(const std::vector<int>& heights) const;

    // Transaction-related RPCs
    RawTransaction get_raw_transaction(const std::string& tx_hash) const;
    std::string get_transaction_raw_json(const std::string& tx_hash) const;

    // Network-related RPCs
    NetworkInfoResponse get_network_info() const;
    BlockchainInfoResponse get_chain_info() const;
    InfoResponse get_info() const;

    // Single-address insight API with correct param format: {"addresses": [addr]}
    AddressBalance get_address_balance_single(const std::string& address) const;
    std::vector<AddressDelta> get_address_deltas_single(const std::string& address, int start_height = 0, int end_height = 0) const;
    std::vector<AddressMempoolTransaction> get_address_mempool_single(const std::string& address) const;

    // Returns {first_height, last_height} for a transparent address; both 0 if never seen.
    std::pair<int,int> get_address_first_last_height(const std::string& address) const;

    // Get current recommended batch size (may be reduced due to fat blocks)
    int get_recommended_batch_size() const;

    // Generic JSON-RPC call - returns raw JSON string
    std::string make_json_rpc_request(const std::string& method,
                                   const glz::generic& params,
                                   Backend backend = Backend::Main) const;

    bool make_json_rpc_request(RpcRequest request, std::string &response_string) const;

    bool make_multi_json_rpc_request(const std::vector<RpcRequest> &multirequest, std::string &response_string,
                                      Backend backend = Backend::Main) const;

private:
    // Singleton instance
    static std::unique_ptr<YcashRpcClient> client_instance;

    std::string rpc_host;
    int rpc_port;
    std::string rpc_user;
    std::string rpc_pass;
    RpcConfig::Config rpc_config;
    mutable CurlHandle curl_main;   // Main CURL handle for indexer
    mutable CurlHandle curl_adhoc;  // Adhoc CURL handle for side tasks
    mutable CurlHandle curl_state;  // CURL handle for state update

    // Get curl handle for the specified backend
    CURL* get_curl_handle_for_backend(Backend backend) const;

    // Dynamic batch sizing state
    mutable std::mutex batch_state_mutex;
    mutable int recommended_batch_size;        // Current recommended batch size
    mutable int configured_batch_size;         // Original batch size from config

    // Helper to initialize persistent connection options
    void initialize_persistent_connection(CURL* handle) const;

    // Perform HTTP call and return raw response body (no Glaze parsing)
    std::string perform_rpc_http(const std::string& method, const glz::generic& params, Backend backend) const;

    // Extract the "result" value from a JSON-RPC envelope as a raw substring (preserves key order)
    static std::string extract_result_raw(const std::string& rpc_envelope);
};
