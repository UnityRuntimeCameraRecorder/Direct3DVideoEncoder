#pragma once

#include <stdexcept>
#include <string>

// Identifies an unimplemented backend independently of its graphics API.
class NotImplementedException final : public std::logic_error
{
public:
    // Creates a descriptive error naming the unavailable implementation.
    explicit NotImplementedException(const std::string& message) : std::logic_error(message) {}
};
