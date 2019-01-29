/*
 * Copyright (c) 2017 Andrzej Krzemienski.
 * Copyright (c) 2015-2018 Dubalu LLC

 * Boost Software License - Version 1.0 - August 17th, 2003
 *
 * Permission is hereby granted, free of charge, to any person or organization
 * obtaining a copy of the software and accompanying documentation covered by
 * this license (the "Software") to use, reproduce, display, distribute,
 * execute, and transmit the Software, and to prepare derivative works of the
 * Software, and to permit third-parties to whom the Software is furnished to
 * do so, all subject to the following:
 *
 * The copyright notices in the Software and this entire statement, including
 * the above license grant, this restriction and the following disclaimer,
 * must be included in all copies of the Software, in whole or in part, and
 * all derivative works of the Software, unless such copies or derivative
 * works are solely in the form of machine-executable object code generated by
 * a source language processor.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
 * SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
 * FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifndef STATIC_STRING_HH
#define STATIC_STRING_HH

#include <cstddef>            // for std::size_t
#include <iostream>
#include <string>             // for std::string
#include "string_view.hh"     // for std::string_view
#include <type_traits>
#include <utility>

#include "cassert.h"          // for ASSERT


namespace static_string {

// # Implementation of a constexpr-compatible assertion
#ifdef NDEBUG
#   define constexpr_assert(CHECK) void(0)
#else
#   define constexpr_assert(CHECK) ((CHECK) ? void(0) : []{ASSERT(!#CHECK);}())
#endif


template <std::size_t N, typename T>
class static_string {
	static_assert(N > 0 && N < 0, "Invalid specialization of string");
};

struct literal_ref {};
template <std::size_t N>
using string_literal_ref = static_string<N, literal_ref>;

struct char_array {};
template <std::size_t N>
using string_char_array = static_string<N, char_array>;


// # A wraper over a string literal with alternate interface. No ownership management

template <std::size_t N>
class static_string<N, literal_ref> {
	const char (&_data)[N + 1];

public:
	constexpr static_string(const char (&data)[N + 1])
	  : _data{(constexpr_assert(data[N] == 0), data)} { }

	constexpr std::size_t length() const { return N; };
	constexpr const char* c_str() const { return _data; }

	constexpr std::size_t size() const { return N; };
	constexpr const char* data() const { return _data; }
	constexpr char operator[](std::size_t i) const { return constexpr_assert(i >= 0 && i < N), _data[i]; }

	template <typename OT, std::size_t ON>
	constexpr bool operator==(const static_string<ON, OT> &other) const {
		if (N != ON) return false;
		for (std::size_t i = 0; i < N; ++i)
			if (_data[i] != other._data[i])
				return false;
		return true;
	}

	template <typename OT, std::size_t ON>
	constexpr bool operator<(const static_string<ON, OT> &other) const {
		for (std::size_t i = 0; i < N && i < ON; ++i) {
			if (_data[i] < other._data[i])
				return true;
			if (_data[i] > other._data[i])
				return false;
		}
		return N < ON;
	}

	constexpr operator const char* () const { return _data; }
	constexpr operator std::string_view() const { return std::string_view(_data, N); }
	operator std::string() const { return std::string(_data, N); }

};


// # A function to convert integers to string.

template <std::size_t N, char... a>
struct chars_to_string {
	constexpr static char value[N + 1]{a..., 0};
};

template <int num, std::size_t N = 0, char... a>
struct explode : explode<num / 10, N + 1, ('0' + num % 10), a...> { };

template <std::size_t N, char... a>
struct explode<0, N, a...> {
	constexpr static char value[N + 1]{a..., 0};
};

template <>
struct explode<0, 0> {
	constexpr static char value[2]{'0', 0};
};

template <int num>
static constexpr auto
to_string() {
	constexpr explode<num> data;
	return string_char_array<sizeof(data.value) - 1>(data.value);
}

template <char ch>
constexpr static auto
char_to_string() {
	return string_char_array<1>(ch);
}


// # This implements a null-terminated array that stores elements on stack.

template <std::size_t N>
class static_string<N, char_array> {
	char _data[N + 1];

	struct private_ctor {};

	template <std::size_t M, std::size_t... Il, std::size_t... Ir, typename TL, typename TR>
	constexpr explicit static_string(private_ctor, const static_string<M, TL>& l, const static_string<N - M, TR>& r, std::integer_sequence<std::size_t, Il...>, std::integer_sequence<std::size_t, Ir...>)
	  : _data{l[Il]..., r[Ir]..., 0} { }

	template <std::size_t... Il, typename T>
	constexpr explicit static_string(private_ctor, const static_string<N, T>& l, std::integer_sequence<std::size_t, Il...>)
	  : _data{l[Il]..., 0} { }

	template <std::size_t... Il>
	constexpr explicit static_string(private_ctor, const char* p, std::size_t size, std::integer_sequence<std::size_t, Il...>)
	  : _data{p[Il]..., 0} { constexpr_assert(N == size); (void)(size); }

public:
	template <std::size_t M, typename TL, typename TR, typename std::enable_if<(M < N), bool>::type = true>
	constexpr explicit static_string(static_string<M, TL> l, static_string<N - M, TR> r)
	  : static_string(private_ctor{}, l, r, std::make_integer_sequence<std::size_t, M>{}, std::make_integer_sequence<std::size_t, N - M>{}) { }

	constexpr static_string(string_literal_ref<N> l) // converting
	  : static_string(private_ctor{}, l, std::make_integer_sequence<std::size_t, N>{}) { }

	constexpr static_string(const char* p, std::size_t size)
	  : static_string(private_ctor{}, p, size, std::make_integer_sequence<std::size_t, N>{}) { }

	constexpr static_string(char ch)
	  : _data{(constexpr_assert(N == 1), ch), 0} { }

	constexpr std::size_t length() const { return N; }
	constexpr const char* c_str() const { return _data; }

	constexpr std::size_t size() const { return N; }
	constexpr const char* data() const { return _data; }

	constexpr char operator[](std::size_t i) const { return constexpr_assert(i >= 0 && i < N), _data[i]; }

	template <typename OT, std::size_t ON>
	constexpr bool operator==(const static_string<ON, OT> &other) const {
		if (N != ON) return false;
		for (std::size_t i = 0; i < N; ++i)
			if (_data[i] != other._data[i])
				return false;
		return true;
	}

	template <typename OT, std::size_t ON>
	constexpr bool operator<(const static_string<ON, OT> &other) const {
		for (std::size_t i = 0; i < N && i < ON; ++i) {
			if (_data[i] < other._data[i])
				return true;
			if (_data[i] > other._data[i])
				return false;
		}
		return N < ON;
	}

	constexpr operator const char* () const { return _data; }
	constexpr operator std::string_view() const { return std::string_view(_data, N); }
	operator std::string() const { return std::string(_data, N); }
};


// # A function that converts raw string literal into string_literal_ref and deduces the size.

template <std::size_t N_PLUS_1>
constexpr static auto
string(const char (&s)[N_PLUS_1]) {
	return string_literal_ref<N_PLUS_1 - 1>(s);
}

constexpr static auto
string(const char ch) {
	return string_char_array<1>(&ch, 1);
}

// template <std::size_t N>
// constexpr static auto
// string(const char* s, std::size_t size) {
// 	return string_char_array<N>(s, size);
// }

// constexpr static auto
// operator"" _ss(const char* s, std::size_t size) {
// 	return string(s, size);
// }


// # A set of concatenating operators, for different combinations of raw literals, string_literal_ref<>, and string_char_array<>

template <std::size_t NL, typename TL, std::size_t NR, typename TR>
constexpr auto operator+(const static_string<NL, TL>& l, const static_string<NR, TR>& r) {
	return string_char_array<NL + NR>(l, r);
}

template <std::size_t NL_PLUS_1, std::size_t NR, typename TR>
constexpr auto operator+(const char (&l)[NL_PLUS_1], const static_string<NR, TR>& r) {
	return string_char_array<NL_PLUS_1 - 1 + NR>(string_literal_ref<NL_PLUS_1 - 1>(l), r);
}

template <std::size_t NL, typename TL, std::size_t NR_PLUS_1>
constexpr auto operator+(const static_string<NL, TL>& l, const char (&r)[NR_PLUS_1]) {
	return string_char_array<NL + NR_PLUS_1 - 1>(l, string_literal_ref<NR_PLUS_1 - 1>(r));
}

template <std::size_t NR, typename TR>
constexpr auto operator+(char l, const static_string<NR, TR>& r) {
	return string_char_array<1 + NR>(string_char_array<1>(l), r);
}

template <std::size_t NL, typename TL>
constexpr auto operator+(const static_string<NL, TL>& l, char r) {
	return string_char_array<NL + 1>(l, string_char_array<1>(r));
}

template <std::size_t NL, typename TL, typename TR>
std::string operator+(const static_string<NL, TL>& l, const TR& r)
{
	std::string result(l.data(), l.size());
	result.append(r);
	return result;
}

template <std::size_t NR, typename TL, typename TR>
std::string operator+(const TL& l, const static_string<NR, TR>& r)
{
	std::string result(r.data(), r.size());
	result.append(l);
	return result;
}

} // namespace static_string

#endif // STATIC_STRING_HH
