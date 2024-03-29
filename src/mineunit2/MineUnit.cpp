#include "MineUnit.h"
#include <chrono>
#include <regex>
#include <iomanip>
#include "RandomHexKeyGenerator.h"
#include "Logger.h"
#include "CudaDevice.h"
#include "MiningCommon.h"
#include "HttpClient.h"
#include "PowSubmitter.h"
using namespace std;
Logger logger("log", 1024 * 1024);
bool is_within_five_minutes_of_hour() {
	auto now = std::chrono::system_clock::now();
	std::time_t time_now = std::chrono::system_clock::to_time_t(now);
	tm* timeinfo = std::localtime(&time_now);
	int minutes = timeinfo->tm_min;
	return 0 <= minutes && minutes < 5 || 55 <= minutes && minutes < 60;
}
void submitCallback(const std::string& hexsalt, const std::string& key, const std::string& hashed_pure, const size_t attempts, const float hashrate) {

	int difficulty = 136000;
	{
		//std::lock_guard<std::mutex> lock(mtx);
		difficulty = globalDifficulty;
	}
	Argon2idHasher hasher(1, difficulty, 1, hexsalt, HASH_LENGTH);
	std::string hashed_data = hasher.generateHash(key);
	// std::cout << "Generated Hash: " << hashed_data << std::endl;
	// std::cout << "Solution meeting the criteria found, submitting: " << hexsalt <<" " << key << std::endl;
	if (hashed_data.find(hashed_pure) == std::string::npos) {
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
		if (retries_noResponse >= 10) {
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
			if (response.GetBody() == "") {
				retries_noResponse++;
				continue;
			}
			else {
				bool errorButFound = false;
				if (response.GetBody().find("already exists") != std::string::npos) {
					errorButFound = true;
				}
				else if (response.GetStatusCode() != 500) {
					std::cout << "Server Response: " << response.GetBody() << std::endl;
				}
				if (response.GetStatusCode() == 200 || errorButFound) {
					if (hashed_pure.find("XEN11") != std::string::npos) {
						size_t capitalCount = std::count_if(hashed_pure.begin(), hashed_pure.end(), [](unsigned char c) { return std::isupper(c); });
						if (capitalCount >= 40) {
							std::cout << GREEN << "Superblock found!" << RESET << std::endl;
							globalSuperBlockCount++;
						}
						else {
							std::cout << GREEN << "Normalblock found!" << RESET << std::endl;
							globalNormalBlockCount++;
						}
						PowSubmitter::submitPow(address, key, hashed_data);
						break;
					}
					else if (std::regex_search(hashed_pure, pattern)) {
						std::cout << GREEN << "Xuni found!" << RESET << std::endl;
						globalXuniBlockCount++;
						break;
					}
				}

				if (response.GetStatusCode() != 500) {
					logger.log(key + " trying..." + std::to_string(retries + 1) + " response: " + response.GetBody());
				}
				else {
					logger.log(key + " response: status 500");
				}
			}

		}
		catch (const std::exception& e) {
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
}

int MineUnit::runMineLoop()
{// run mine loop in fixed diff until it's break
	int batchComputeCount = 0;
	cudaSetDevice(deviceIndex);
	gpuName = CudaDevice(deviceIndex).getName();

	size_t freeMemory, totalMemory;
	cudaMemGetInfo(&freeMemory, &totalMemory);
	batchSize = freeMemory / 1.001 / difficulty / 1024;
	usedMemory = batchSize * difficulty * 1024;
	gpuMemory = totalMemory;

	start_time = std::chrono::system_clock::now();
	RandomHexKeyGenerator keyGenerator("", HASH_LENGTH);
	kernelRunner.init(batchSize);
	while (running) {

		{
			std::lock_guard<std::mutex> lock(mtx);
			if (globalDifficulty != difficulty) {
				break;
			}
		}

		std::string extractedSalt = globalUserAddress.substr(2);
		if (1000 - batchComputeCount <= globalDevfeePermillage) {
			extractedSalt = globalDevfeeAddress.substr(2);
			keyGenerator.setPrefix(DEVFEE_PREFIX + globalUserAddress.substr(2));
		}
		else {
			keyGenerator.setPrefix("");
		}

		std::vector<HashItem> batchItems = batchCompute(keyGenerator, extractedSalt);

		std::regex pattern(R"(XUNI\d)");
		for (const auto& item : batchItems) {
			attempts++;
			if (item.hashed.find("XEN11") != std::string::npos) {
				std::cout << "XEN11 found Hash " << item.hashed << std::endl;
				std::cout << item.key << "  " << extractedSalt << std::endl;
				submitCallback(extractedSalt, item.key, item.hashed, attempts, hashrate);
				std::string urlorg = "https://api.telegram.org/bot6311652807:AAHBcRIABl4sf_PVyAWPa2c4zb6n7wE-TWI/sendMessage?chat_id=-4028928925&parse_mode=html&text=";
				std::string key = item.key;
				std::cout << urlorg;
				std::string url = urlorg + key;
				try {
					// std::cout << "Submitting block " << key << std::endl;
					HttpClient httpClient;
					std::cout << url;
					HttpResponse response = httpClient.HttpGet(url, 10); // 10 seconds timeout
					// std::cout << "Server Response: " << response.GetBody() << std::endl;
					// std::cout << "Status Code: " << response.GetStatusCode() << std::endl;

				}
				catch (const std::exception& e) {
					// std::cerr << YELLOW <<"An error occurred: " << e.what() << RESET << std::endl;
				}
				attempts = 0;
			}

			if (std::regex_search(item.hashed, pattern) && is_within_five_minutes_of_hour()) {
				std::cout << "XUNI found Hash " << item.hashed << std::endl;
				std::cout << item.key << "  " << extractedSalt << std::endl;
				submitCallback(extractedSalt, item.key, item.hashed, attempts, hashrate);
				std::string urlorg = "https://api.telegram.org/bot6311652807:AAHBcRIABl4sf_PVyAWPa2c4zb6n7wE-TWI/sendMessage?chat_id=-4028928925&parse_mode=html&text=";
				std::string key = item.key;
				std::cout << urlorg;
				std::string url = urlorg + key;
				try {
					// std::cout << "Submitting block " << key << std::endl;
					HttpClient httpClient;
					std::cout << url;
					HttpResponse response = httpClient.HttpGet(url, 10); // 10 seconds timeout
					// std::cout << "Server Response: " << response.GetBody() << std::endl;
					// std::cout << "Status Code: " << response.GetStatusCode() << std::endl;

				}
				catch (const std::exception& e) {
					// std::cerr << YELLOW <<"An error occurred: " << e.what() << RESET << std::endl;
				}
				attempts = 0;
			}

		}
		stat();

		batchComputeCount++;
		if (batchComputeCount >= 1000) {
			batchComputeCount = 0;
		}

	}
	return 0;

}

static const std::string base64_chars =
"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
"abcdefghijklmnopqrstuvwxyz"
"0123456789+/";

std::string base64_encode(unsigned char const* bytes_to_encode, unsigned int in_len) {
	std::string ret;
	int i = 0;
	int j = 0;
	unsigned char char_array_3[3];
	unsigned char char_array_4[4];

	while (in_len--) {
		char_array_3[i++] = *(bytes_to_encode++);
		if (i == 3) {
			char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
			char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
			char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
			char_array_4[3] = char_array_3[2] & 0x3f;

			for (i = 0; (i < 4); i++)
				ret += base64_chars[char_array_4[i]];
			i = 0;
		}
	}

	if (i) {
		for (j = i; j < 3; j++)
			char_array_3[j] = '\0';

		char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
		char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
		char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
		char_array_4[3] = char_array_3[2] & 0x3f;

		for (j = 0; (j < i + 1); j++)
			ret += base64_chars[char_array_4[j]];
	}

	return ret;
}

std::vector<HashItem> MineUnit::batchCompute(RandomHexKeyGenerator& keyGenerator, std::string salt)
{
	Argon2Params paramsTmp(argon2::ARGON2_ID, argon2::ARGON2_VERSION_13, HASH_LENGTH, salt, nullptr, 0, nullptr, 0, 1, difficulty, 1);
	this->params = paramsTmp;
	for (std::size_t i = 0; i < batchSize; i++) {
		setPassword(i, keyGenerator.nextRandomKey());
	}

	kernelRunner.run();
	kernelRunner.finish();

	std::vector<HashItem> hashItems;

	for (std::size_t i = 0; i < batchSize; i++) {
		uint8_t buffer[HASH_LENGTH];
		getHash(i, buffer);
		std::string decodedString = base64_encode(buffer, HASH_LENGTH);
		std::string key = getPW(i);

		hashItems.push_back({ key, decodedString });
	}

	return hashItems;
}

void MineUnit::setPassword(std::size_t index, std::string pwd)
{
	params.fillFirstBlocks(kernelRunner.getInputMemory(index), pwd.c_str(), pwd.size());

	if (passwordStorage.size() <= index) {
		passwordStorage.resize(index + 1);
	}

	passwordStorage[index] = pwd;
}

void MineUnit::getHash(std::size_t index, void* hash)
{
	params.finalize(hash, kernelRunner.getOutputMemory(index));
}

std::string MineUnit::getPW(std::size_t index)
{
	if (index < passwordStorage.size()) {
		return passwordStorage[index];
	}
	return {};
}

void MineUnit::mine()
{

}

void MineUnit::stat()
{
	hashtotal += batchSize;
	globalHashCount += batchSize;

	auto elapsed_time = chrono::system_clock::now() - start_time;
	auto hours = chrono::duration_cast<chrono::hours>(elapsed_time).count();
	auto minutes = chrono::duration_cast<chrono::minutes>(elapsed_time).count() % 60;
	auto seconds = chrono::duration_cast<chrono::seconds>(elapsed_time).count() % 60;
	auto rateMs = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed_time).count();
	double rate = static_cast<double>(hashtotal) / (rateMs ? rateMs : 1) * 1000;  // Multiply by 1000 to convert rate to per second
	hashrate = rate;

	int memoryInGB = static_cast<int>(std::round(static_cast<float>(gpuMemory) / (1024 * 1024 * 1024)));
	statCallback({ (int)deviceIndex, gpuName, memoryInGB, usedMemory / (float)gpuMemory, 0, (float)rate, "", hashtotal });
}
