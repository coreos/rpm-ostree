
#pragma once

#include <string>
#include <unordered_map>
#include "rust/cxx.h"

namespace rpmostreecxx
{
  struct string_cache
  {
    using cached_str_t = size_t;  // result of std::hash<std::string_view>

    inline const std::string&
    as_string (cached_str_t cstr) const
      {
        return str_cache.at (cstr);
      }

    inline std::string_view
    as_view (cached_str_t cstr) const { return as_string (cstr); }

    inline rust::Str
    as_rstr (cached_str_t cstr) const { return as_string (cstr); }

    inline rust::String
    as_rstring (cached_str_t cstr) const { return as_string (cstr); }

    inline cached_str_t
    get_or_insert (std::string_view str)
      {
        cached_str_t result = std::hash<std::string_view>{} (str);
        if (str_cache.find (result) == str_cache.end ())
          str_cache[result] = std::string (str);
        return result;
      }

  private:
    std::unordered_map<cached_str_t, std::string> str_cache;
  };

  using cached_string = string_cache::cached_str_t;
}
