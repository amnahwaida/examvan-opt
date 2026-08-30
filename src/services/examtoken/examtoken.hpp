#pragma once
#include <string>
#include <optional>
#include "models/exam.hpp"

namespace examvan::examtoken {

std::string hash_token(const std::string& token);
bool verify_token(const std::string& token, const std::string& hash);
std::string generate_active_token(const std::string& base, const std::string& mode);
bool matches(const models::Exam& exam, const std::string& token);

} // namespace examvan::examtoken
