#pragma once

#include <mutex>
#include <atomic>
#include <functional>
#include <string>
#include <map>

constexpr std::size_t HASH_LENGTH = 64;
constexpr std::size_t MAX_SUBMIT_RETRIES = 5;

const std::string CONFIG_FILENAME = "config.txt";
const std::string DEVFEE_PREFIX = "FFFFFFFF";

extern std::string globalUserAddress;
extern std::string globalDevfeeAddress;
extern std::atomic<int> globalDevfeePermillage; // per 1000
extern std::string machineId;

extern std::atomic<int> globalDifficulty;
extern std::mutex mtx;
extern std::atomic<bool> running;
extern std::mutex coutmtx;

extern std::atomic<int> globalNormalBlockCount;
extern std::atomic<int> globalSuperBlockCount;
extern std::atomic<int> globalXuniBlockCount;

extern std::chrono::system_clock::time_point start_time;
extern std::atomic<long> globalHashCount;

struct gpuInfo
{
	int index;
	std::string name;
	int memory;
	float usingMemory;
	int temperature;
	float hashrate;
	std::string power;
	size_t hashCount;
};
extern std::map<int, std::pair<gpuInfo, std::chrono::steady_clock::time_point>> globalGpuInfos;
extern std::mutex globalGpuInfosMutex;

using SubmitCallback = std::function<void(const std::string& hexsalt, const std::string& key, const std::string& hashed_pure, const size_t attempts, const float hashrate)>;
using StatCallback = std::function<void(const gpuInfo gpuinfo)>;




const std::string RED = "\033[31m";
const std::string GREEN = "\033[32m";
const std::string YELLOW = "\033[33m";
const std::string RESET = "\033[0m";
