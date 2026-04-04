#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ui {

class Message {
public:
    enum class Type { Text, Binary };

    explicit Message(std::string text)
        : type_(Type::Text), text_(std::move(text)) {}

    explicit Message(std::vector<uint8_t> data)
        : type_(Type::Binary), binary_(std::move(data)) {}

    bool is_text()   const { return type_ == Type::Text;   }
    bool is_binary() const { return type_ == Type::Binary; }

    std::string_view            text()   const { return text_;   }
    const std::vector<uint8_t>& data()   const { return binary_; }

private:
    Type                 type_;
    std::string          text_;
    std::vector<uint8_t> binary_;
};

} // namespace ui