/*
 * mod-true-companion — AzerothCore True Companion module
 * Copyright (C) 2026 mod-true-companion contributors
 * Licensed under the GNU GPL v2 or (at your option) any later version. See LICENSE.
 *
 * Small, dependency-free JSON value type with a hardened parser.
 * Used for Ollama requests/responses, the addon protocol and persisted profiles.
 * The parser enforces a maximum nesting depth and input size so malformed or
 * hostile input (addon messages, model output) cannot exhaust the stack.
 */

#ifndef MOD_TRUE_COMPANION_JSON_H
#define MOD_TRUE_COMPANION_JSON_H

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace truecompanion
{
    struct JsonParseLimits
    {
        size_t maxBytes = 1024 * 1024;
        int maxDepth = 32;
    };

    class Json
    {
    public:
        enum class Type : uint8_t { Null, Bool, Number, String, Array, Object };

        using Array = std::vector<Json>;
        using Object = std::map<std::string, Json>;

        Json() : _type(Type::Null) { }
        Json(std::nullptr_t) : _type(Type::Null) { }
        Json(bool b) : _type(Type::Bool), _bool(b) { }
        Json(int v) : _type(Type::Number), _num(v) { }
        Json(unsigned v) : _type(Type::Number), _num(v) { }
        Json(int64_t v) : _type(Type::Number), _num(double(v)) { }
        Json(uint64_t v) : _type(Type::Number), _num(double(v)) { }
        Json(double v) : _type(Type::Number), _num(v) { }
        Json(char const* s) : _type(Type::String), _str(s ? s : "") { }
        Json(std::string s) : _type(Type::String), _str(std::move(s)) { }
        Json(Array a) : _type(Type::Array), _arr(std::make_shared<Array>(std::move(a))) { }
        Json(Object o) : _type(Type::Object), _obj(std::make_shared<Object>(std::move(o))) { }

        static Json MakeArray() { return Json(Array{}); }
        static Json MakeObject() { return Json(Object{}); }

        Type GetType() const { return _type; }
        bool IsNull() const { return _type == Type::Null; }
        bool IsBool() const { return _type == Type::Bool; }
        bool IsNumber() const { return _type == Type::Number; }
        bool IsString() const { return _type == Type::String; }
        bool IsArray() const { return _type == Type::Array; }
        bool IsObject() const { return _type == Type::Object; }

        bool AsBool(bool def = false) const { return _type == Type::Bool ? _bool : def; }
        double AsNumber(double def = 0.0) const { return _type == Type::Number ? _num : def; }
        int64_t AsInt(int64_t def = 0) const { return _type == Type::Number ? int64_t(_num) : def; }
        std::string const& AsString() const;
        std::string AsString(std::string const& def) const { return _type == Type::String ? _str : def; }

        Array const& AsArray() const;
        Object const& AsObject() const;

        // Mutable access; converts Null into the requested container type.
        Array& MutArray();
        Object& MutObject();

        // Object helpers
        bool Has(std::string const& key) const;
        Json const& operator[](std::string const& key) const; // returns Null if missing
        Json& Set(std::string const& key, Json value);
        void Push(Json value);
        size_t Size() const;

        // Typed getters with defaults (object only)
        std::string GetString(std::string const& key, std::string const& def = "") const;
        double GetNumber(std::string const& key, double def = 0.0) const;
        int64_t GetInt(std::string const& key, int64_t def = 0) const;
        bool GetBool(std::string const& key, bool def = false) const;

        std::string Dump() const;

        using ParseLimits = JsonParseLimits;

        // Returns std::nullopt on any syntax error; `error` (if given) receives a reason.
        static std::optional<Json> Parse(std::string const& text, std::string* error = nullptr,
                                         ParseLimits limits = ParseLimits());

        static std::string Escape(std::string const& s);

    private:
        void DumpTo(std::string& out) const;

        Type _type;
        bool _bool = false;
        double _num = 0.0;
        std::string _str;
        std::shared_ptr<Array> _arr;
        std::shared_ptr<Object> _obj;
    };
}

#endif
