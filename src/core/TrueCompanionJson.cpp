/*
 * mod-true-companion — Licensed under the GNU GPL v2 or later. See LICENSE.
 */

#include "TrueCompanionJson.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace truecompanion
{
    namespace
    {
        std::string const kEmptyString;
        Json::Array const kEmptyArray;
        Json::Object const kEmptyObject;
        Json const kNull;

        void AppendUtf8(std::string& out, uint32_t cp)
        {
            if (cp < 0x80)
                out.push_back(char(cp));
            else if (cp < 0x800)
            {
                out.push_back(char(0xC0 | (cp >> 6)));
                out.push_back(char(0x80 | (cp & 0x3F)));
            }
            else if (cp < 0x10000)
            {
                out.push_back(char(0xE0 | (cp >> 12)));
                out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(char(0x80 | (cp & 0x3F)));
            }
            else
            {
                out.push_back(char(0xF0 | (cp >> 18)));
                out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
                out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(char(0x80 | (cp & 0x3F)));
            }
        }

        class Parser
        {
        public:
            Parser(std::string const& s, Json::ParseLimits l) : _s(s), _limits(l) { }

            std::optional<Json> Run(std::string* error)
            {
                if (_s.size() > _limits.maxBytes)
                    return Fail(error, "input too large");
                SkipWs();
                Json v;
                if (!ParseValue(v, 0))
                    return Fail(error, _err.empty() ? "syntax error" : _err);
                SkipWs();
                if (_p != _s.size())
                    return Fail(error, "trailing characters");
                return v;
            }

        private:
            std::optional<Json> Fail(std::string* error, std::string const& msg)
            {
                if (error)
                    *error = msg + " at offset " + std::to_string(_p);
                return std::nullopt;
            }

            void SkipWs()
            {
                while (_p < _s.size() && (_s[_p] == ' ' || _s[_p] == '\t' || _s[_p] == '\n' || _s[_p] == '\r'))
                    ++_p;
            }

            bool Literal(char const* lit)
            {
                size_t n = 0;
                while (lit[n])
                    ++n;
                if (_s.compare(_p, n, lit) != 0)
                    return false;
                _p += n;
                return true;
            }

            bool ParseValue(Json& out, int depth)
            {
                if (depth > _limits.maxDepth)
                {
                    _err = "nesting too deep";
                    return false;
                }
                if (_p >= _s.size())
                    return false;
                char c = _s[_p];
                switch (c)
                {
                    case 'n': if (Literal("null")) { out = Json(); return true; } return false;
                    case 't': if (Literal("true")) { out = Json(true); return true; } return false;
                    case 'f': if (Literal("false")) { out = Json(false); return true; } return false;
                    case '"': { std::string s; if (!ParseString(s)) return false; out = Json(std::move(s)); return true; }
                    case '[': return ParseArray(out, depth);
                    case '{': return ParseObject(out, depth);
                    default:
                        if (c == '-' || (c >= '0' && c <= '9'))
                            return ParseNumber(out);
                        return false;
                }
            }

            bool ParseNumber(Json& out)
            {
                size_t start = _p;
                if (_s[_p] == '-')
                    ++_p;
                bool digits = false;
                while (_p < _s.size() && _s[_p] >= '0' && _s[_p] <= '9') { ++_p; digits = true; }
                if (_p < _s.size() && _s[_p] == '.')
                {
                    ++_p;
                    while (_p < _s.size() && _s[_p] >= '0' && _s[_p] <= '9') { ++_p; digits = true; }
                }
                if (_p < _s.size() && (_s[_p] == 'e' || _s[_p] == 'E'))
                {
                    ++_p;
                    if (_p < _s.size() && (_s[_p] == '+' || _s[_p] == '-'))
                        ++_p;
                    while (_p < _s.size() && _s[_p] >= '0' && _s[_p] <= '9')
                        ++_p;
                }
                if (!digits)
                    return false;
                std::string tok = _s.substr(start, _p - start);
                char* end = nullptr;
                double v = std::strtod(tok.c_str(), &end);
                if (!end || *end != '\0' || !std::isfinite(v))
                    return false;
                out = Json(v);
                return true;
            }

            bool Hex4(uint32_t& v)
            {
                if (_p + 4 > _s.size())
                    return false;
                v = 0;
                for (int i = 0; i < 4; ++i)
                {
                    char c = _s[_p++];
                    v <<= 4;
                    if (c >= '0' && c <= '9') v |= uint32_t(c - '0');
                    else if (c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10);
                    else return false;
                }
                return true;
            }

            bool ParseString(std::string& out)
            {
                ++_p; // opening quote
                while (_p < _s.size())
                {
                    char c = _s[_p++];
                    if (c == '"')
                        return true;
                    if (static_cast<unsigned char>(c) < 0x20)
                        return false;
                    if (c != '\\')
                    {
                        out.push_back(c);
                        continue;
                    }
                    if (_p >= _s.size())
                        return false;
                    char e = _s[_p++];
                    switch (e)
                    {
                        case '"': out.push_back('"'); break;
                        case '\\': out.push_back('\\'); break;
                        case '/': out.push_back('/'); break;
                        case 'b': out.push_back('\b'); break;
                        case 'f': out.push_back('\f'); break;
                        case 'n': out.push_back('\n'); break;
                        case 'r': out.push_back('\r'); break;
                        case 't': out.push_back('\t'); break;
                        case 'u':
                        {
                            uint32_t cp;
                            if (!Hex4(cp))
                                return false;
                            if (cp >= 0xD800 && cp <= 0xDBFF)
                            {
                                uint32_t lo;
                                if (_p + 2 > _s.size() || _s[_p] != '\\' || _s[_p + 1] != 'u')
                                    return false;
                                _p += 2;
                                if (!Hex4(lo) || lo < 0xDC00 || lo > 0xDFFF)
                                    return false;
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            }
                            else if (cp >= 0xDC00 && cp <= 0xDFFF)
                                return false;
                            AppendUtf8(out, cp);
                            break;
                        }
                        default:
                            return false;
                    }
                }
                return false;
            }

            bool ParseArray(Json& out, int depth)
            {
                ++_p;
                Json::Array arr;
                SkipWs();
                if (_p < _s.size() && _s[_p] == ']')
                {
                    ++_p;
                    out = Json(std::move(arr));
                    return true;
                }
                while (true)
                {
                    SkipWs();
                    Json v;
                    if (!ParseValue(v, depth + 1))
                        return false;
                    arr.push_back(std::move(v));
                    SkipWs();
                    if (_p >= _s.size())
                        return false;
                    if (_s[_p] == ',') { ++_p; continue; }
                    if (_s[_p] == ']') { ++_p; break; }
                    return false;
                }
                out = Json(std::move(arr));
                return true;
            }

            bool ParseObject(Json& out, int depth)
            {
                ++_p;
                Json::Object obj;
                SkipWs();
                if (_p < _s.size() && _s[_p] == '}')
                {
                    ++_p;
                    out = Json(std::move(obj));
                    return true;
                }
                while (true)
                {
                    SkipWs();
                    if (_p >= _s.size() || _s[_p] != '"')
                        return false;
                    std::string key;
                    if (!ParseString(key))
                        return false;
                    SkipWs();
                    if (_p >= _s.size() || _s[_p] != ':')
                        return false;
                    ++_p;
                    SkipWs();
                    Json v;
                    if (!ParseValue(v, depth + 1))
                        return false;
                    obj[key] = std::move(v);
                    SkipWs();
                    if (_p >= _s.size())
                        return false;
                    if (_s[_p] == ',') { ++_p; continue; }
                    if (_s[_p] == '}') { ++_p; break; }
                    return false;
                }
                out = Json(std::move(obj));
                return true;
            }

            std::string const& _s;
            Json::ParseLimits _limits;
            size_t _p = 0;
            std::string _err;
        };
    }

    std::string const& Json::AsString() const { return _type == Type::String ? _str : kEmptyString; }
    Json::Array const& Json::AsArray() const { return (_type == Type::Array && _arr) ? *_arr : kEmptyArray; }
    Json::Object const& Json::AsObject() const { return (_type == Type::Object && _obj) ? *_obj : kEmptyObject; }

    Json::Array& Json::MutArray()
    {
        if (_type != Type::Array)
        {
            *this = Json(Array{});
        }
        else if (_arr.use_count() > 1)
            _arr = std::make_shared<Array>(*_arr); // copy-on-write
        return *_arr;
    }

    Json::Object& Json::MutObject()
    {
        if (_type != Type::Object)
        {
            *this = Json(Object{});
        }
        else if (_obj.use_count() > 1)
            _obj = std::make_shared<Object>(*_obj); // copy-on-write
        return *_obj;
    }

    bool Json::Has(std::string const& key) const
    {
        return _type == Type::Object && _obj && _obj->count(key) > 0;
    }

    Json const& Json::operator[](std::string const& key) const
    {
        if (_type != Type::Object || !_obj)
            return kNull;
        auto it = _obj->find(key);
        return it == _obj->end() ? kNull : it->second;
    }

    Json& Json::Set(std::string const& key, Json value)
    {
        MutObject()[key] = std::move(value);
        return *this;
    }

    void Json::Push(Json value) { MutArray().push_back(std::move(value)); }

    size_t Json::Size() const
    {
        if (_type == Type::Array && _arr) return _arr->size();
        if (_type == Type::Object && _obj) return _obj->size();
        return 0;
    }

    std::string Json::GetString(std::string const& key, std::string const& def) const
    {
        Json const& v = (*this)[key];
        return v.IsString() ? v.AsString() : def;
    }

    double Json::GetNumber(std::string const& key, double def) const
    {
        Json const& v = (*this)[key];
        return v.IsNumber() ? v.AsNumber() : def;
    }

    int64_t Json::GetInt(std::string const& key, int64_t def) const
    {
        Json const& v = (*this)[key];
        return v.IsNumber() ? v.AsInt() : def;
    }

    bool Json::GetBool(std::string const& key, bool def) const
    {
        Json const& v = (*this)[key];
        if (v.IsBool())
            return v.AsBool();
        if (v.IsNumber())
            return v.AsNumber() != 0.0;
        return def;
    }

    std::string Json::Escape(std::string const& s)
    {
        std::string out;
        out.reserve(s.size() + 8);
        for (char ch : s)
        {
            unsigned char c = static_cast<unsigned char>(ch);
            switch (c)
            {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                default:
                    if (c < 0x20)
                    {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out += buf;
                    }
                    else
                        out.push_back(ch);
            }
        }
        return out;
    }

    void Json::DumpTo(std::string& out) const
    {
        switch (_type)
        {
            case Type::Null: out += "null"; break;
            case Type::Bool: out += _bool ? "true" : "false"; break;
            case Type::Number:
            {
                if (!std::isfinite(_num))
                {
                    out += "0";
                    break;
                }
                double ip;
                if (std::modf(_num, &ip) == 0.0 && std::fabs(_num) < 9.0e15)
                {
                    out += std::to_string(int64_t(_num));
                }
                else
                {
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "%.6g", _num);
                    out += buf;
                }
                break;
            }
            case Type::String:
                out.push_back('"');
                out += Escape(_str);
                out.push_back('"');
                break;
            case Type::Array:
            {
                out.push_back('[');
                bool first = true;
                for (Json const& v : AsArray())
                {
                    if (!first) out.push_back(',');
                    first = false;
                    v.DumpTo(out);
                }
                out.push_back(']');
                break;
            }
            case Type::Object:
            {
                out.push_back('{');
                bool first = true;
                for (auto const& [k, v] : AsObject())
                {
                    if (!first) out.push_back(',');
                    first = false;
                    out.push_back('"');
                    out += Escape(k);
                    out += "\":";
                    v.DumpTo(out);
                }
                out.push_back('}');
                break;
            }
        }
    }

    std::string Json::Dump() const
    {
        std::string out;
        DumpTo(out);
        return out;
    }

    std::optional<Json> Json::Parse(std::string const& text, std::string* error, ParseLimits limits)
    {
        Parser p(text, limits);
        return p.Run(error);
    }
}
