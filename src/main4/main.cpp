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
        MineUnit unit(deviceIndex, globalDifficulty, statCallback);
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

    //AppConfig appConfig(CONFIG_FILENAME);
    //appConfig.load();
    //globalUserAddress = "0xaD51a4e1507204b46e0e269E2CAc126447a54435";
    globalUserAddress = argv[1];
    globalDevfeePermillage = 0;
    std::cout << GREEN << "Logged in as " << globalUserAddress << ". Devfee set at " << globalDevfeePermillage << "/1000." << RESET << std::endl;

    machineId = getMachineId();
    std::cout << "Machine ID: " << machineId << std::endl;

    globalDifficulty = atoi(argv[2]);
    std::cout << " Wallet Address: " << globalUserAddress <<" Difficutly: " << globalDifficulty << std::endl;
    //updateDifficulty();
    //std::thread difficultyThread(updateDifficultyPeriodically);
    //difficultyThread.detach();

    std::thread uploadThread(uploadGpuInfos);
    uploadThread.detach();

    std::thread submitThread(workerThread);
    submitThread.detach();

    Logger logger("log", 1024 * 1024);
    
    StatCallback statCallback = [](const gpuInfo gpuinfo)
    {
        {
            std::lock_guard<std::mutex> lock(globalGpuInfosMutex);
            globalGpuInfos[gpuinfo.index] = {gpuinfo, std::chrono::steady_clock::now()};
        }
        int difficulty = 136000;
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
        std::thread t(runMiningOnDevice, i, statCallback);
        t.detach();
    }

    while (running)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    std::cout << std::endl;
    return 0;
}
