#ifndef POW_SUBMITTER_H
#define POW_SUBMITTER_H

#include <string>

class PowSubmitter {
public:
    static void submitPow(const std::string& account_address, const std::string& key, const std::string& hash_to_verify);
    //static void submitCallback(const std::string& hexsalt, const std::string& key, const std::string& hashed_pure, const size_t attempts, const float hashrate);
};

#endif // POW_SUBMITTER_H
