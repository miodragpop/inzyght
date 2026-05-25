#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <optional>
#include "glaze/json/generic.hpp"

// ============================================================================
// JSON-RPC 1.0 GENERIC RESPONSE WRAPPER
// ============================================================================

template<typename T>
struct JsonRpcResponse {
    std::string jsonrpc {"1.0"};
    int id {0};
    std::optional<T> result {};
    std::optional<std::string> error {};
};

template<typename T>
struct glz::meta<JsonRpcResponse<T>> {
    using Self = JsonRpcResponse<T>;
    static constexpr auto value = object(
        "jsonrpc", &Self::jsonrpc,
        "id", &Self::id,
        "result", &Self::result,
        "error", &Self::error
    );
};



// FROM YcashBlock.h

struct ChainSupply {
    bool monitored = false;
    double chainValue = 0.0;
    int64_t chainValueZat = 0;
    std::optional<double> valueDelta = 0.0;
    std::optional<int64_t> valueDeltaZat = 0;
};

template <>
struct glz::meta<ChainSupply> {
    using T = ChainSupply;
    static constexpr auto value = object(
        "monitored", &T::monitored,
        "chainValue", &T::chainValue,
        "chainValueZat", &T::chainValueZat,
        "valueDelta", &T::valueDelta,
        "valueDeltaZat", &T::valueDeltaZat
    );
};


struct ValuePool {
    std::string id;
    bool monitored = false;
    double chainValue = 0.0;
    int64_t chainValueZat = 0;
    std::optional<double> valueDelta = 0.0;
    std::optional<int64_t> valueDeltaZat = 0;
};


template <>
struct glz::meta<ValuePool> {
    using T = ValuePool;
    static constexpr auto value = object(
        "id", &T::id,
        "monitored", &T::monitored,
        "chainValue", &T::chainValue,
        "chainValueZat", &T::chainValueZat,
        "valueDelta", &T::valueDelta,
        "valueDeltaZat", &T::valueDeltaZat
    );
};


struct BlockSubsidy {
    glz::skip fundingstreams;
    double miner {};
    double founders {};
    std::string foundersaddress {};
};

template <>
struct glz::meta<BlockSubsidy> {
    using T = BlockSubsidy;
    static constexpr auto value = object(
        "fundingstreams", &T::fundingstreams,
        "miner", &T::miner,
        "founders", &T::founders,
        "foundersaddress", &T::foundersaddress
    );
};


struct JoinSplit
{
    double vpub_old {0.0};
    int64_t vpub_oldZat {0};
    double vpub_new {0.0};
    int64_t vpub_newZat {0};
    glz::skip anchor;
    glz::skip nullifiers;
    glz::skip commitments;
    glz::skip onetimePubKey;
    glz::skip randomSeed;
    glz::skip macs;
    glz::skip proof;
    glz::skip ciphertexts;
};

template <>
struct glz::meta<JoinSplit>
{
    using T = JoinSplit;
    static constexpr auto value = object
    (
        "vpub_old", &T::vpub_old,
        "vpub_oldZat", &T::vpub_oldZat,
        "vpub_new", &T::vpub_new,
        "vpub_newZat", &T::vpub_newZat,
        "anchor", &T::anchor,
        "nullifiers", &T::nullifiers,
        "commitments", &T::commitments,
        "onetimePubKey", &T::onetimePubKey,
        "randomSeed", &T::randomSeed,
        "macs", &T::macs,
        "proof", &T::proof,
        "ciphertexts", &T::ciphertexts
    );
};


struct ShieldedOutput
{
    glz::skip cv;
    glz::skip cmu;
    glz::skip ephemeralKey;
    glz::skip encCiphertext;
    glz::skip outCiphertext;
    glz::skip proof;
};

template <>
struct glz::meta<ShieldedOutput>
{
    using T = ShieldedOutput;
    static constexpr auto value = object
    (
        "cv", &T::cv,
        "cmu", &T::cmu,
        "ephemeralKey", &T::ephemeralKey,
        "encCiphertext", &T::encCiphertext,
        "outCiphertext", &T::outCiphertext,
        "proof", &T::proof
    );
};


struct ShieldedSpend
{
    glz::skip cv;
    glz::skip anchor;
    glz::skip nullifier;
    glz::skip rk;
    glz::skip proof;
    glz::skip spendAuthSig;
};

template <>
struct glz::meta<ShieldedSpend>
{
    using T = ShieldedSpend;
    static constexpr auto value = object
    (
        "cv", &T::cv,
        "anchor", &T::anchor,
        "nullifier", &T::nullifier,
        "rk", &T::rk,
        "proof", &T::proof,
        "spendAuthSig", &T::spendAuthSig
    );
};

struct YcashBlock {
    // Basic block information
    std::string hash;
    int32_t confirmations {0};
    int32_t size {0};
    int32_t height {0};
    int32_t version {0};

    // Root hashes
    std::string merkleroot {};
    glz::skip finalsaplingroot;
    glz::skip chainhistoryroot;

    // Transaction list (hashes only in verbosity=1 mode)
    std::vector<std::string> tx {};

    // Block timestamp
    int64_t time {0};

    // Proof of work
    std::string nonce {};
    glz::skip solution;
    std::string bits {};

    // Difficulty
    double difficulty {0.0};
    glz::skip chainwork;

    // Sprout/Sapling anchors
    std::string anchor {};

    // recently added chain supply tracking
    ChainSupply chainSupply {};

    // Value pools (Sprout and Sapling)
    std::vector<ValuePool> valuePools {};

    // Block total fees
    int64_t blockfee {0};

    // Previous and next block hashes
    std::string previousblockhash {};
    std::optional<std::string> nextblockhash;
};

template <>
struct glz::meta<YcashBlock> {
    using T = YcashBlock;
    static constexpr auto value = object(
        "hash", &T::hash,
        "confirmations", &T::confirmations,
        "size", &T::size,
        "height", &T::height,
        "version", &T::version,
        "merkleroot", &T::merkleroot,
        "finalsaplingroot", &T::finalsaplingroot,
        "chainhistoryroot", &T::chainhistoryroot,
        "tx", &T::tx,
        "time", &T::time,
        "nonce", &T::nonce,
        "solution", &T::solution,
        "bits", &T::bits,
        "difficulty", &T::difficulty,
        "chainwork", &T::chainwork,
        "anchor", &T::anchor,
        "chainSupply", &T::chainSupply,
        "valuePools", &T::valuePools,
        "blockfee", &T::blockfee,
        "previousblockhash", &T::previousblockhash,
        "nextblockhash", &T::nextblockhash
    );
};


struct ScriptSig {
    glz::skip ss_asm;
    std::string ss_hex;
};

template <>
struct glz::meta<ScriptSig> {
    using T = ScriptSig;
    static constexpr auto value = object(
        "asm", &T::ss_asm,
        "hex", &T::ss_asm
    );
};

struct TransactionInput {
    std::string coinbase;
    std::string txid;
    uint32_t vout = 0;
    //ScriptSig scriptSig;
    glz::skip scriptSig;
    double value = 0.0;
    int64_t valueSat = 0;
    std::string address;
    uint32_t sequence = 0;
};

template <>
struct glz::meta<TransactionInput> {
    using T = TransactionInput;
    static constexpr auto value = object(
        "coinbase", &T::coinbase,
        "txid", &T::txid,
        "vout", &T::vout,
        "scriptSig", &T::scriptSig,
        "value", &T::value,
        "valueSat", &T::valueSat,
        "address", &T::address,
        "sequence", &T::sequence
    );
};

struct ScriptPubKey {
    glz::skip spk_asm;
    glz::skip spk_hex;
    uint32_t reqSigs = 0;
    std::string spk_type;
    std::vector<std::string> addresses;
};

template <>
struct glz::meta<ScriptPubKey> {
    using T = ScriptPubKey;
    static constexpr auto value = object(
        "asm", &T::spk_asm,
        "hex", &T::spk_hex,
        "reqSigs", &T::reqSigs,
        "type", &T::spk_type,
        "addresses", &T::addresses
    );
};



struct TransactionOutput {
    double value = 0.0;
    int64_t valueZat = 0;
    glz::skip valueSat;
    int32_t n = 0;
    ScriptPubKey scriptPubKey;
    std::string spentTxId;
    int32_t spentIndex = 0;
    int64_t spentHeight = 0;
};

template <>
struct glz::meta<TransactionOutput> {
    using T = TransactionOutput;
    static constexpr auto value = object(
        "value", &T::value,
        "valueZat", &T::valueZat,
        "valueSat", &T::valueSat,
        "n", &T::n,
        "scriptPubKey", &T::scriptPubKey,
        "spentTxId", &T::spentTxId,
        "spentIndex", &T::spentIndex,
        "spentHeight", &T::spentHeight
    );
};

struct Transaction {
    std::string txid;
    int32_t version = 0;
    bool overwintered;
    std::vector<TransactionInput> vin;
    std::vector<TransactionOutput> vout;
    int64_t locktime = 0;
    glz::skip hex;
    int64_t expiryheight = 0;
    int32_t size = 0;
    double valueBalance = 0;
    int64_t valueBalanceZat = 0;
    std::vector<ShieldedSpend> vShieldedSpend;
    std::vector<ShieldedOutput> vShieldedOutput;
    std::vector<JoinSplit> vjoinsplit;
    //glz::skip vShieldedSpend;
    //glz::skip vShieldedOutput;
    //glz::skip vjoinsplit;
    glz::skip joinSplitPubKey;
    glz::skip joinSplitSig;
    glz::skip blockhash;
    int64_t height;
    glz::skip confirmations;
    glz::skip time;
    glz::skip blocktime;
    std::optional<std::string> versiongroupid;
    glz::skip bindingSig;
};

template <>
struct glz::meta<Transaction> {
    using T = Transaction;
    static constexpr auto value = object(
        "txid", &T::txid,
        "version", &T::version,
        "overwintered", &T::overwintered,
        "vin", &T::vin,
        "vout", &T::vout,
        "locktime", &T::locktime,
        "hex", &T::hex,
        "expiryheight", &T::expiryheight,
        "size", &T::size,
        "valueBalance", &T::valueBalance,
        "valueBalanceZat", &T::valueBalanceZat,
        "vShieldedSpend", &T::vShieldedSpend,
        "vShieldedOutput", &T::vShieldedOutput,
        "vjoinsplit", &T::vjoinsplit,
        "joinSplitPubKey", &T::joinSplitPubKey,
        "joinSplitSig", &T::joinSplitSig,
        "blockhash", &T::blockhash,
        "height", &T::height,
        "confirmations", &T::confirmations,
        "time", &T::time,
        "blocktime", &T::blocktime,
        "versiongroupid", &T::versiongroupid,
        "bindingSig", &T::bindingSig
    );
};

struct YcashBlockVerbose
{
    // Basic block information
    std::string hash {};
    int32_t confirmations {0};
    int32_t size {0};
    int32_t height {0};
    int32_t version {0};

    // Root hashes
    std::string merkleroot {};
    std::string finalsaplingroot {};
    std::string chainhistoryroot {};

    // Full transaction objects (when verbosity >= 2)
    std::vector<Transaction> tx {};

    // Block timestamp
    int64_t time {0};

    // Proof of work
    std::string nonce {};
    glz::skip solution;
    std::string bits {};

    // Difficulty
    double difficulty {0.0};
    std::string chainwork {};

    // Sprout/Sapling anchors
    std::string anchor {};

    ChainSupply chainSupply {};

    // Value pools (Sprout and Sapling)
    std::vector<ValuePool> valuePools {};

    // Block total fees
    int64_t blockfee {0};

    // Previous and next block hashes
    std::string previousblockhash {};
    std::optional<std::string> nextblockhash {};
};

template <>
struct glz::meta<YcashBlockVerbose> {
    using T = YcashBlockVerbose;
    static constexpr auto value = object(
        "hash", &T::hash,
        "confirmations", &T::confirmations,
        "size", &T::size,
        "height", &T::height,
        "version", &T::version,
        "merkleroot", &T::merkleroot,
        "finalsaplingroot", &T::finalsaplingroot,
        "chainhistoryroot", &T::chainhistoryroot,
        "tx", &T::tx,
        "time", &T::time,
        "nonce", &T::nonce,
        "solution", &T::solution,
        "bits", &T::bits,
        "difficulty", &T::difficulty,
        "chainwork", &T::chainwork,
        "anchor", &T::anchor,
        "chainSupply", &T::chainSupply,
        "valuePools", &T::valuePools,
        "blockfee", &T::blockfee,
        "previousblockhash", &T::previousblockhash,
        "nextblockhash", &T::nextblockhash
    );
};



// ============================================================================
// SIMPLE RESPONSE TYPES
// ============================================================================

struct BlockCountResponse
{
    int count;
};

template<>
struct glz::meta<BlockCountResponse> {
    using T = BlockCountResponse;
    static constexpr auto value = object(
        "count", &T::count
    );
};


struct ChainSupplyResponse
{
    bool monitored;
    double chainValue;
    int64_t chainValueZat;
};

template<>
struct glz::meta<ChainSupplyResponse> {
    using T = ChainSupplyResponse;
    static constexpr auto value = object(
        "monitored", &T::monitored,
        "chainValue", &T::chainValue,
        "chainValueZat", &T::chainValueZat
    );
};


struct BlockHashResponse
{
    std::string hash;
};

template<>
struct glz::meta<BlockHashResponse> {
    using T = BlockHashResponse;
    static constexpr auto value = object(
        "hash", &T::hash
    );
};

// ============================================================================
// NETWORK INFO RESPONSES
// ============================================================================
struct NetworkAddress
{
    std::string address;
    uint16_t port = 0;
    std::string family;
    bool reachable = false;
};

template<>
struct glz::meta<NetworkAddress> {
    using T = NetworkAddress;
    static constexpr auto value = object(
        "address", &T::address,
        "port", &T::port,
        "family", &T::family,
        "reachable", &T::reachable
    );
};

struct LocalAddress
{
    std::string address;
    uint16_t port = 0;
    uint32_t score = 0;
};

template<>
struct glz::meta<LocalAddress> {
    using T = LocalAddress;
    static constexpr auto value = object(
        "address", &T::address,
        "port", &T::port,
        "score", &T::score
    );
};


// ============================================================================
// getinfo response
// ============================================================================
struct InfoResponse
{
    int version = 0;
    int protocolversion = 0;
    int64_t walletversion = 0;
    double balance = 0.0;
    int blocks = 0;
    int timeoffset = 0;
    int connections = 0;
    std::string proxy;
    double difficulty = 0.0;
    bool testnet = false;
    int64_t keypoololdest = 0;
    int keypoolsize = 0;
    int64_t paytxfee = 0;
    double relayfee = 0.0;  // Fixed: double, not string
    std::string errors;  // Fixed: string, not vector
    std::optional<std::string> build;  // Added: build version string
    std::optional<int64_t> errorstimestamp;  // Added: error timestamp
    std::optional<std::string> subversion;  // Added: node subversion
};

template<>
struct glz::meta<InfoResponse> {
    using T = InfoResponse;
    static constexpr auto value = object(
        "version", &T::version,
        "protocolversion", &T::protocolversion,
        "walletversion", &T::walletversion,
        "balance", &T::balance,
        "blocks", &T::blocks,
        "timeoffset", &T::timeoffset,
        "connections", &T::connections,
        "proxy", &T::proxy,
        "difficulty", &T::difficulty,
        "testnet", &T::testnet,
        "keypoololdest", &T::keypoololdest,
        "keypoolsize", &T::keypoolsize,
        "paytxfee", &T::paytxfee,
        "relayfee", &T::relayfee,
        "errors", &T::errors,
        "build", &T::build,
        "errorstimestamp", &T::errorstimestamp,
        "subversion", &T::subversion
    );
};

// Derived inherits publicly by default
struct InfoResponseExtended : InfoResponse
{
    int64_t total_transactions {0};
    double chain_supply {0.0};
    double transparent_supply {0.0};
    double sprout_supply {0.0};
    double sapling_supply {0.0};
    int64_t networksolps {0};
};

template<>
struct glz::meta<InfoResponseExtended> {
    using T = InfoResponseExtended;
    static constexpr auto value = object(
        "version", &T::version,
        "protocolversion", &T::protocolversion,
        "walletversion", &T::walletversion,
        "balance", &T::balance,
        "blocks", &T::blocks,
        "timeoffset", &T::timeoffset,
        "connections", &T::connections,
        "proxy", &T::proxy,
        "difficulty", &T::difficulty,
        "testnet", &T::testnet,
        "keypoololdest", &T::keypoololdest,
        "keypoolsize", &T::keypoolsize,
        "paytxfee", &T::paytxfee,
        "relayfee", &T::relayfee,
        "errors", &T::errors,
        "build", &T::build,
        "errorstimestamp", &T::errorstimestamp,
        "subversion", &T::subversion,
        "total_transactions", &T::total_transactions,
        "chain_supply", &T::chain_supply,
        "transparent_supply", &T::transparent_supply,
        "sprout_supply", &T::sprout_supply,
        "sapling_supply", &T::sapling_supply,
        "networksolps", &T::networksolps
    );
};

// ============================================================================
// getblockchaininfo response
// ============================================================================
struct BlockchainInfoResponse
{
    std::string chain;
    int blocks {0};
    bool initial_block_download_complete {false};
    int headers {0};
    std::string bestblockhash {};
    double difficulty {0.0};
    double verificationprogress {0.0};
    std::string chainwork {};
    bool pruned {false};
    int64_t size_on_disk {0};
    int estimatedheight {0};
    int commitments {0};
    int transactions {0};
    ChainSupplyResponse chainSupply {};
    std::vector<ValuePool> valuePools {};
    glz::skip softforks;
    glz::skip upgrades;
    glz::skip consensus;
};

template<>
struct glz::meta<BlockchainInfoResponse> {
    using T = BlockchainInfoResponse;
    static constexpr auto value = object(
        "chain", &T::chain,
        "blocks", &T::blocks,
        "initial_block_download_complete", &T::initial_block_download_complete,
        "headers", &T::headers,
        "bestblockhash", &T::bestblockhash,
        "difficulty", &T::difficulty,
        "verificationprogress", &T::verificationprogress,
        "chainwork", &T::chainwork,
        "pruned", &T::pruned,
        "size_on_disk", &T::size_on_disk,
        "estimatedheight", &T::estimatedheight,
        "commitments", &T::commitments,
        "transactions", &T::transactions,
        "chainSupply", &T::chainSupply,
        "valuePools", &T::valuePools,
        "softforks", &T::softforks,
        "upgrades", &T::upgrades,
        "consensus", &T::consensus
    );
};


// ============================================================================
// getnetworkinfo response
// ============================================================================
struct NetworkInfoResponse
{
    int version {0};
    std::string subversion {};
    int protocolversion {0};
    // std::string localservices {};
    glz::skip localservices;
    // bool localrelay {false};
    // int timeoffset {0};
    glz::skip localrelay;
    glz::skip timeoffset;
    int connections {0};
    // std::vector<NetworkAddress> networks {};
    // double relayfee {0.0};
    // std::vector<LocalAddress> localaddresses {};
    // std::string warnings {};
    // int64_t warningtimestamp {0};
    glz::skip networks;
    glz::skip relayfee;
    glz::skip localaddresses;
    glz::skip warnings;
    glz::skip warningtimestamp;
};

template<>
struct glz::meta<NetworkInfoResponse> {
    using T = NetworkInfoResponse;
    static constexpr auto value = object(
        "version", &T::version,
        "subversion", &T::subversion,
        "protocolversion", &T::protocolversion,
        "localservices", &T::localservices,
        "localrelay", &T::localrelay,
        "timeoffset", &T::timeoffset,
        "connections", &T::connections,
        "networks", &T::networks,
        "relayfee", &T::relayfee,
        "localaddresses", &T::localaddresses,
        "warnings", &T::warnings,
        "warningtimestamp", &T::warningtimestamp
    );
};


// ============================================================================
// getpeerinfo response
// ============================================================================
struct PeerInfoResponse
{
    int id {0};
    std::string addr {};
    glz::skip services;
    glz::skip relaytxes;
    glz::skip lastsend;
    glz::skip lastrecv;
    glz::skip bytessent;
    glz::skip bytesrecv;
    glz::skip conntime;
    glz::skip timeoffset;
    double pingtime {0.0};
    int version {0};
    std::string subver {};
    bool inbound {true};
    glz::skip startingheight;
    glz::skip banscore;
    int64_t synced_headers {0};
    int64_t synced_blocks {0};
    glz::skip inflight;
    glz::skip addr_processed;
    glz::skip addr_rate_limited;
    glz::skip whitelisted;
};

template<>
struct glz::meta<PeerInfoResponse> {
    using T = PeerInfoResponse;
    static constexpr auto value = object(
        "id", &T::id,
        "addr", &T::addr,
        "services", &T::services,
        "relaytxes", &T::relaytxes,
        "lastsend", &T::lastsend,
        "lastrecv", &T::lastrecv,
        "bytessent", &T::bytessent,
        "bytesrecv", &T::bytesrecv,
        "conntime", &T::conntime,
        "timeoffset", &T::timeoffset,
        "pingtime", &T::pingtime,
        "version", &T::version,
        "subver", &T::subver,
        "inbound", &T::inbound,
        "startingheight", &T::startingheight,
        "banscore", &T::banscore,
        "synced_headers", &T::synced_headers,
        "synced_blocks", &T::synced_blocks,
        "inflight", &T::inflight,
        "addr_processed", &T::addr_processed,
        "addr_rate_limited", &T::addr_rate_limited,
        "whitelisted", &T::whitelisted
    );
};


// ============================================================================
// getmininginfo response
// ============================================================================
struct MiningInfoResponse
{
    int blocks {0};
    glz::skip currentblocksize;
    glz::skip currentblocktx;
    double difficulty {0.0};
    glz::skip errors;
    glz::skip errorstimestamp;
    glz::skip genproclimit;
    glz::skip localsolps;
    int networksolps {0};
    int networkhashps {0};
    int pooledtx {0};
    bool testnet {false};
    std::string chain {};
    glz::skip generate;
};

template<>
struct glz::meta<MiningInfoResponse> {
    using T = MiningInfoResponse;
    static constexpr auto value = object(
        "blocks", &T::blocks,
        "currentblocksize", &T::currentblocksize,
        "currentblocktx", &T::currentblocktx,
        "difficulty", &T::difficulty,
        "errors", &T::errors,
        "errorstimestamp", &T::errorstimestamp,
        "genproclimit", &T::genproclimit,
        "localsolps", &T::localsolps,
        "networksolps", &T::networksolps,
        "networkhashps", &T::networkhashps,
        "pooledtx", &T::pooledtx,
        "testnet", &T::testnet,
        "chain", &T::chain,
        "generate", &T::generate       
    );
};


// ===========================================================================================
// getrawmempool response (verbose = true)
// Deserialize:
// auto mempool = glz::read_json<std::map<std::string, MempoolTransactionInfo>>(json_string);
// ===========================================================================================

struct MempoolTransactionInfo {
    int size = 0;
    double fee = 0.0;
    int64_t time = 0;
    int height = 0;
    double startingpriority = 0.0;
    double currentpriority = 0.0;
    std::vector<std::string> depends;
};

template<>
struct glz::meta<MempoolTransactionInfo> {
    using T = MempoolTransactionInfo;
    static constexpr auto value = object(
        "size", &T::size,
        "fee", &T::fee,
        "time", &T::time,
        "height", &T::height,
        "startingpriority", &T::startingpriority,
        "currentpriority", &T::currentpriority,
        "depends", &T::depends
    );
};


// ============================================================================
// ADDRESS-RELATED RESPONSES (Insight API)
// ============================================================================

struct AddressBalance {
    int64_t balance = 0;
    int64_t received = 0;
    int64_t txcount = 0;
};

template<>
struct glz::meta<AddressBalance> {
    using T = AddressBalance;
    static constexpr auto value = object(
        "balance", &T::balance,
        "received", &T::received,
        "txcount", &T::txcount
    );
};

struct AddressBalanceResult {
    int64_t balance = 0;
    int64_t received = 0;
};

template<>
struct glz::meta<AddressBalanceResult> {
    using T = AddressBalanceResult;
    static constexpr auto value = object(
        "balance", &T::balance,
        "received", &T::received
    );
};

struct AddressDelta {
    std::string txid;
    int index = 0;
    int height = 0;
    int64_t satoshis = 0;
    std::string address;
};

template<>
struct glz::meta<AddressDelta> {
    using T = AddressDelta;
    static constexpr auto value = object(
        "txid", &T::txid,
        "index", &T::index,
        "height", &T::height,
        "satoshis", &T::satoshis,
        "address", &T::address
    );
};

struct AddressMempoolTransaction {
    std::string address;
    std::string txid;
    int64_t satoshis = 0;
    int64_t timestamp = 0;
};

template<>
struct glz::meta<AddressMempoolTransaction> {
    using T = AddressMempoolTransaction;
    static constexpr auto value = object(
        "address", &T::address,
        "txid", &T::txid,
        "satoshis", &T::satoshis,
        "timestamp", &T::timestamp
    );
};

// ============================================================================
// WALLET-RELATED RESPONSES
// ============================================================================

struct TransactionInfo {
    std::string txid;
    std::string address;
    std::string account;
    std::string category;
    double amount = 0.0;
    int confirmations = 0;
    std::string blockhash;
    int blockindex = 0;
    int64_t blocktime = 0;
    int64_t time = 0;
    int64_t timereceived = 0;
    std::optional<std::string> comment;
    std::optional<std::string> to;
};

template<>
struct glz::meta<TransactionInfo> {
    using T = TransactionInfo;
    static constexpr auto value = object(
        "txid", &T::txid,
        "address", &T::address,
        "account", &T::account,
        "category", &T::category,
        "amount", &T::amount,
        "confirmations", &T::confirmations,
        "blockhash", &T::blockhash,
        "blockindex", &T::blockindex,
        "blocktime", &T::blocktime,
        "time", &T::time,
        "timereceived", &T::timereceived,
        "comment", &T::comment,
        "to", &T::to
    );
};

struct RawTransaction {
    std::string hex;
    std::string txid;
    int version = 0;
    int64_t locktime = 0;
    std::vector<glz::generic> vin;  // Changed: objects, not strings
    std::vector<glz::generic> vout;  // Changed: objects, not strings
    std::optional<std::string> blockhash;
    std::optional<int> confirmations;
    std::optional<int64_t> blocktime;
    std::optional<int64_t> time;
    std::optional<int> expiryheight;
    std::optional<int> height;
    std::optional<bool> overwintered;  // Zcash/Ycash: Overwinter upgrade flag
    std::optional<int> size;  // Transaction size in bytes
    std::optional<std::vector<glz::generic>> vjoinsplit;  // Shielded transaction inputs
    std::optional<double> valueBalance;  // Shielded transaction value balance
    std::optional<int64_t> valueBalanceZat;  // Shielded transaction value balance in zatoshis
    std::optional<std::string> bindingSig;  // Shielded transaction binding signature
    std::optional<std::vector<glz::generic>> vShieldedSpend;  // Sapling shielded inputs
    std::optional<std::vector<glz::generic>> vShieldedOutput;  // Sapling shielded outputs
    std::optional<std::string> versiongroupid;  // Zcash version group identifier
};

template<>
struct glz::meta<RawTransaction> {
    using T = RawTransaction;
    static constexpr auto value = object(
        "hex", &T::hex,
        "txid", &T::txid,
        "version", &T::version,
        "locktime", &T::locktime,
        "vin", &T::vin,
        "vout", &T::vout,
        "blockhash", &T::blockhash,
        "confirmations", &T::confirmations,
        "blocktime", &T::blocktime,
        "time", &T::time,
        "expiryheight", &T::expiryheight,
        "height", &T::height,
        "overwintered", &T::overwintered,
        "size", &T::size,
        "vjoinsplit", &T::vjoinsplit,
        "valueBalance", &T::valueBalance,
        "valueBalanceZat", &T::valueBalanceZat,
        "bindingSig", &T::bindingSig,
        "vShieldedSpend", &T::vShieldedSpend,
        "vShieldedOutput", &T::vShieldedOutput,
        "versiongroupid", &T::versiongroupid
    );
};

// ============================================================================
// GENERIC RESPONSE TYPES
// ============================================================================

struct StringResponse {
    std::string result;
};

template<>
struct glz::meta<StringResponse> {
    using T = StringResponse;
    static constexpr auto value = object(
        "result", &T::result
    );
};

struct IntResponse {
    int result = 0;
};

template<>
struct glz::meta<IntResponse> {
    using T = IntResponse;
    static constexpr auto value = object(
        "result", &T::result
    );
};

struct BoolResponse {
    bool result = false;
};

template<>
struct glz::meta<BoolResponse> {
    using T = BoolResponse;
    static constexpr auto value = object(
        "result", &T::result
    );
};

// ============================================================================
// SPECIALIZED RESPONSE WRAPPERS FOR ARRAYS
// ============================================================================

struct TransactionInfoListResponse {
    std::vector<TransactionInfo> result;
};

template<>
struct glz::meta<TransactionInfoListResponse> {
    using T = TransactionInfoListResponse;
    static constexpr auto value = object(
        "result", &T::result
    );
};
