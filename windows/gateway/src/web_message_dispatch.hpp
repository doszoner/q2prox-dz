#pragma once

#include <memory>

template <typename Deleter, typename Callback>
void with_owned_web_message(wchar_t *value, Deleter deleter, Callback callback) {
    std::unique_ptr<wchar_t, Deleter> owned(value, deleter);
    callback(owned.get());
}
