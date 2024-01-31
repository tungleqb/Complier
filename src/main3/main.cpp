#include <iostream>
#include <fstream>
#include <thread>
#include <string>
#include <chrono>
#include <csignal>
#include <iomanip>
#include <map>
#include <regex>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <sstream>
#include <cuda_runtime.h>

#include "MiningCommon.h"
#include "CudaDevice.h"
#include "MineUnit.h"
#include "AppConfig.h"
#include "Logger.h"
#include "Argon2idHasher.h"
#include <nlohmann/json.hpp>
#include "HttpClient.h"
#include "PowSubmitter.h"
#include "SHA256Hasher.h"
#include "RandomHexKeyGenerator.h"
using namespace std;
/*
std::string getDifficulty()
{
    //HttpClient httpClient;

    try
    {
        HttpResponse response = httpClient.HttpGet("http://xenblocks.io/difficulty", 10); // 10 seconds timeout
        if (response.GetStatusCode() != 200)
        {
            throw std::runtime_error("Failed to get the difficulty: HTTP status code " + std::to_string(response.GetStatusCode()));
        }

        auto json_response = nlohmann::json::parse(response.GetBody());
        return json_response["difficulty"].get<std::string>();
    }
    catch (const nlohmann::json::parse_error &e)
    {
        throw std::runtime_error("JSON parsing error: " + std::string(e.what()));
    }
    catch (const std::exception &e)
    {
        throw std::runtime_error("Error: " + std::string(e.what()));
    }
}
*/
void updateDifficulty()
{
    try
    {
        std::ifstream file("difficulty.txt");
if (file.is_open()) {
    int new_difficulty;
    if (file >> new_difficulty) { // read difficulty
        std::lock_guard<std::mutex> lock(mtx);
        if (globalDifficulty != new_difficulty) {
            globalDifficulty = new_difficulty; // update difficulty
            std::cout << "Updated difficulty to " << new_difficulty << std::endl;
        }
    }
    file.close();
}
else {
    std::cerr << "The local difficult.txt file was not recognized" << std::endl;
}
    }
    catch (const std::exception &e)
    {
        // std::cerr << YELLOW << "Error updating difficulty: " << e.what() << RESET << std::endl;
    }
}

void updateDifficultyPeriodically()
{
    while (running)
    {
        updateDifficulty();
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
}

nlohmann::json vectorToJson(const std::string &machineId, const std::string &accountAddress, const std::vector<std::pair<int, gpuInfo>> &data)
{
    nlohmann::json j;
    nlohmann::json gpuArray = nlohmann::json::array();

    for (const auto &item : data)
    {
        nlohmann::json jItem;
        std::ostringstream os;

        jItem["index"] = item.first;
        jItem["name"] = item.second.name;
        jItem["memory"] = item.second.memory;

        os << std::fixed << std::setprecision(2) << item.second.usingMemory * 100;
        jItem["usingMemory"] = os.str();
        jItem["temperature"] = item.second.temperature;

        os.str("");
        os.clear();
        os << std::fixed << std::setprecision(2) << item.second.hashrate;
        jItem["hashrate"] = os.str();
        jItem["power"] = item.second.power;
        jItem["hashCount"] = item.second.hashCount;
        gpuArray.push_back(jItem);
    }

    j["machineId"] = machineId;
    j["accountAddress"] = accountAddress;
    j["gpuInfos"] = gpuArray;

    return j;
}

void uploadGpuInfos()
{
    while (running)
    {
        auto now = std::chrono::steady_clock::now();
        std::map<int, std::pair<gpuInfo, std::chrono::steady_clock::time_point>> gpuinfos;
        {
            std::lock_guard<std::mutex> lock(globalGpuInfosMutex);
            gpuinfos = globalGpuInfos;
        }
        std::vector<std::pair<int, gpuInfo>> gpuInfos;
        for (const auto &kv : gpuinfos)
        {
            auto duration = std::chrono::duration_cast<std::chrono::minutes>(now - kv.second.second);
            if (duration.count() <= 2)
            {
                gpuInfos.push_back({kv.first, kv.second.first});
            }
        }
        if (gpuInfos.size() == 0)
        {
            std::this_thread::sleep_for(std::chrono::minutes(5));
            continue;
        }
        std::string infoJson = vectorToJson(machineId, globalUserAddress, gpuInfos).dump(-1);
        // std::cout << infoJson << std::endl;
        std::this_thread::sleep_for(std::chrono::minutes(5));
    }
}

std::string getMachineId()
{
    std::ifstream file("/proc/cpuinfo");
    std::string line;
    SHA256Hasher hasher;
    while (std::getline(file, line))
    {
        if (line.find("serial") != std::string::npos)
        {
            return hasher.sha256(line).substr(0, 16);
        }
    }
    RandomHexKeyGenerator keyGenerator;
    return hasher.sha256(keyGenerator.nextRandomKey()).substr(0, 16);
}

void runMiningOnDevice(int deviceIndex,
                       SubmitCallback submitCallback,
                       StatCallback statCallback)
{
    cudaError_t cudaStatus = cudaSetDevice(deviceIndex);
    if (cudaStatus != cudaSuccess)
    {
        std::cerr << "cudaSetDevice failed for device index: " << deviceIndex << std::endl;
        return;
    }
    auto devices = CudaDevice::getAllDevices();
    auto device = devices[deviceIndex];
    // std::cout << "Starting mining on device #" << deviceIndex << ": "
    //           << device.getName() << std::endl;

    while (running)
    {
        MineUnit unit(deviceIndex, globalDifficulty, submitCallback, statCallback);
        if (unit.runMineLoop() < 0)
        {
            std::cerr << "Mining loop failed on device #" << deviceIndex << std::endl;
            break;
        }
    }
}
std::mutex mtx_submit;
std::condition_variable cv;
std::queue<std::function<void()>> taskQueue;
void interruptSignalHandler(int signum)
{
    running = false;
    cv.notify_all();
}
void workerThread() {
    while (running) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mtx_submit);
            cv.wait(lock, []{ return !running || !taskQueue.empty(); });

            if (!running && taskQueue.empty()) {
                break;
            }

            task = std::move(taskQueue.front());
            taskQueue.pop();
        }

        task();
    }
}
int main(int, const char *const *argv)
{
    signal(SIGINT, interruptSignalHandler);

    AppConfig appConfig(CONFIG_FILENAME);
    appConfig.load();
    globalUserAddress = appConfig.getAccountAddress();
    globalDevfeePermillage = appConfig.getDevfeePermillage();
    std::cout << GREEN << "Logged in as " << globalUserAddress << ". Devfee set at " << globalDevfeePermillage << "/1000." << RESET << std::endl;

    machineId = getMachineId();
    std::cout << "Machine ID: " << machineId << std::endl;

    globalDifficulty = 1000;
    updateDifficulty();
    std::thread difficultyThread(updateDifficultyPeriodically);
    difficultyThread.detach();

    std::thread uploadThread(uploadGpuInfos);
    uploadThread.detach();

    std::thread submitThread(workerThread);
    submitThread.detach();

    Logger logger("log", 1024 * 1024);
    SubmitCallback submitCallback = [&logger](const std::string &hexsalt, const std::string &key, const std::string &hashed_pure, const size_t attempts, const float hashrate) {

        std::function<void()> task = [&logger, hexsalt, key, hashed_pure, attempts, hashrate]()
                                 {
            int difficulty = 40404;
            {
                std::lock_guard<std::mutex> lock(mtx);
                difficulty = globalDifficulty;
            }
            Argon2idHasher hasher(1, difficulty, 1, hexsalt, HASH_LENGTH);
            std::string hashed_data = hasher.generateHash(key);
            // std::cout << "Generated Hash: " << hashed_data << std::endl;
            // std::cout << "Solution meeting the criteria found, submitting: " << hexsalt <<" " << key << std::endl;
            if(hashed_data.find(hashed_pure) == std::string::npos) {
                // std::cout << "Hashed data does not match" << std::endl;
                return;
            }
            std::ostringstream hashrateStream;
            hashrateStream << std::fixed << std::setprecision(2) << hashrate;
            std::string address = "0x" + hexsalt;
            nlohmann::json payload = {
                {"hash_to_verify", hashed_data},
                {"key", key},
                {"account", address},
                {"attempts", std::to_string(attempts)},
                {"hashes_per_second", hashrateStream.str()},
                {"worker", "1"}
            };
            std::cout << std::endl;
            std::cout << "Payload: " << payload.dump(4) << std::endl;
            logger.log(payload.dump(-1));

            int retries = 0;
            int retries_noResponse = 0;
            std::regex pattern(R"(XUNI\d)");
            while (true) {
                if(retries_noResponse >= 10) {
                    std::cout << RED << "No response from server after " << retries_noResponse << " retries" << RESET << std::endl;
                    logger.log("No response from server: " + payload.dump(-1));
                    return;
                }
                try {
                    // std::cout << "Submitting block " << key << std::endl;
                    HttpClient httpClient;
                    HttpResponse response = httpClient.HttpPost("http://xenblocks.io/verify", payload, 10); // 10 seconds timeout
                    // std::cout << "Server Response: " << response.GetBody() << std::endl;
                    // std::cout << "Status Code: " << response.GetStatusCode() << std::endl;
                    if(response.GetBody() == "") {
                        retries_noResponse++;
                        continue;
                    } else {
                        bool errorButFound = false;
                        if(response.GetBody().find("already exists") != std::string::npos) {
                            errorButFound = true;
                        } else if(response.GetStatusCode() != 500) {
                            std::cout << "Server Response: " << response.GetBody() << std::endl;
                        }
                        if (response.GetStatusCode() == 200 || errorButFound) {
                            if(hashed_pure.find("XEN11") != std::string::npos){
                                size_t capitalCount = std::count_if(hashed_pure.begin(), hashed_pure.end(), [](unsigned char c) { return std::isupper(c); });
                                if (capitalCount >= 40) {
                                    std::cout << GREEN << "Superblock found!" << RESET << std::endl;
                                    globalSuperBlockCount++;
                                } else {
                                    std::cout << GREEN << "Normalblock found!" << RESET << std::endl;
                                    globalNormalBlockCount++;
                                }
                                PowSubmitter::submitPow(address, key, hashed_data);
                                break;
                            } else if (std::regex_search(hashed_pure, pattern)){
                                std::cout << GREEN << "Xuni found!" << RESET << std::endl;
                                globalXuniBlockCount++;
                                break;
                            }
                        }

                        if (response.GetStatusCode() != 500) {
                            logger.log(key + " trying..." + std::to_string(retries + 1) + " response: " + response.GetBody());
                        } else {
                            logger.log(key + " response: status 500");
                        }
                    }

                } catch (const std::exception& e) {
                    // std::cerr << YELLOW <<"An error occurred: " << e.what() << RESET << std::endl;
                } 
                retries++;
                // std::cout << YELLOW << "Retrying... (" << retries << "/" << MAX_SUBMIT_RETRIES << ")" << RESET << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(2));
                if (retries >= MAX_SUBMIT_RETRIES) {
                    std::cout << RED << "Failed to submit block after " << retries << " retries" << RESET << std::endl;
                    logger.log("Failed to submit block: " + payload.dump(-1));
                    return;
                } 
            }
        };

        {
            std::lock_guard<std::mutex> lock(mtx_submit);
            taskQueue.push(std::move(task));
        }
        cv.notify_one();
    };

    StatCallback statCallback = [](const gpuInfo gpuinfo)
    {
        {
            std::lock_guard<std::mutex> lock(globalGpuInfosMutex);
            globalGpuInfos[gpuinfo.index] = {gpuinfo, std::chrono::steady_clock::now()};
        }
        int difficulty = 40404;
        {
            std::lock_guard<std::mutex> lock(mtx);
            difficulty = globalDifficulty;
        }
        size_t totalHashCount = 0;
        float totalHashrate = 0.0;

        auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(globalGpuInfosMutex);
            int gpuCount = 0;
            for (const auto &kv : globalGpuInfos)
            {
                auto duration = std::chrono::duration_cast<std::chrono::minutes>(now - kv.second.second);
                if (duration.count() > 2)
                {
                    continue;
                }
                gpuCount++;
                const gpuInfo &info = kv.second.first;
                totalHashCount += info.hashCount;
                totalHashrate += info.hashrate;
            }

            std::ostringstream stream;
            auto elapsed_time = chrono::system_clock::now() - start_time;
	        auto hours = chrono::duration_cast<chrono::hours>(elapsed_time).count();
            auto minutes = chrono::duration_cast<chrono::minutes>(elapsed_time).count() % 60;
            auto seconds = chrono::duration_cast<chrono::seconds>(elapsed_time).count() % 60;
            stream << "\033[2K\r"
                   << "Mining: " << globalHashCount << " Hashes [";
            if (hours > 0) {
                stream << hours << ":";
            }
            stream  << std::setw(2) << std::setfill('0') << minutes << ":";
            stream << std::setw(2) << std::setfill('0') << seconds << ", ";
            stream << gpuCount << " GPUs, ";
            if(globalSuperBlockCount > 0) {
                stream << RED  << " super:" << globalSuperBlockCount<< RESET << ", " ;
            }
            if(globalNormalBlockCount > 0) {
                stream << GREEN << "normal:"  << globalNormalBlockCount << RESET << ", " ;
            }
            if(globalXuniBlockCount > 0) {
                stream << YELLOW << "xuni:"  << globalXuniBlockCount << RESET << ", " ;
            }
            stream << std::fixed << std::setprecision(2) << totalHashrate << " Hashes/s, "
                   << "Difficulty=" << difficulty << "]";
            std::string logMessage = stream.str();
            Logger::logToConsole(logMessage);
        }
        // std::cout << "GPU #" << gpuinfo.index << ": " << gpuinfo.name << std::endl;
        // std::cout << "Memory: " << gpuinfo.memory << "GB" << std::endl;
        // std::cout << "Using Memory: " << gpuinfo.usingMemory * 100 << "%" << std::endl;
        // std::cout << "Temperature: " << gpuinfo.temperature << "C" << std::endl;
        // std::cout << "Hashrate: " << gpuinfo.hashrate << "H/s" << std::endl;
        // std::cout << "Power: " << gpuinfo.power << "W" << std::endl;
        // std::cout << "Hash Count: " << gpuinfo.hashCount << std::endl;
    };
    int deviceCount;
    cudaError_t cudaStatus = cudaGetDeviceCount(&deviceCount);
    if (cudaStatus != cudaSuccess)
    {
        std::cerr << "cudaGetDeviceCount failed! Do you have a CUDA-capable GPU installed?" << std::endl;
        return -1;
    }

    auto devices = CudaDevice::getAllDevices();

    std::size_t i = 0;
    for (auto &device : devices)
    {
        std::cout << "Device #" << i << ": "
                  << device.getName() << std::endl;
        i++;
    }
    start_time = std::chrono::system_clock::now();
    for (std::size_t i = 0; i < devices.size(); ++i)
    {
        std::thread t(runMiningOnDevice, i, submitCallback, statCallback);
        t.detach();
    }

    while (running)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    std::cout << std::endl;
    return 0;
}
