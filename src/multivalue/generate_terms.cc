/*
 * Copyright (C) 2015,2016,2017 deipi.com LLC and contributors. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "generate_terms.h"

#include <bitset>             // for bitset
#include <map>                // for __map_iterator, map, operator!=
#include <unordered_set>      // for unordered_set
#include <utility>            // for pair, make_pair

#include "database_utils.h"
#include "datetime.h"         // for tm_t, timegm, to_tm_t
#include "stl_serialise.h"    // for RangeList
#include "utils.h"            // for toUType


const char ctype_date    = required_spc_t::get_ctype(FieldType::DATE);
const char ctype_geo     = required_spc_t::get_ctype(FieldType::GEO);
const char ctype_integer = required_spc_t::get_ctype(FieldType::INTEGER);


inline static bool isnotSubtrixel(std::string& last_valid, uint64_t id_trixel) {
	auto res = std::bitset<SIZE_BITS_ID>(id_trixel).to_string();
	res.assign(res.substr(res.find('1')));
	if (res.find(last_valid) == 0) {
		return false;
	} else {
		last_valid.assign(res);
		return true;
	}
}


void
GenerateTerms::integer(Xapian::Document& doc, const std::vector<uint64_t>& accuracy, const std::vector<std::string>& acc_prefix, int64_t value)
{
	auto it = acc_prefix.begin();
	for (const auto& acc : accuracy) {
		auto term_v = Serialise::integer(value - modulus(value, acc));
		doc.add_term(prefixed(term_v, *it++, ctype_integer));
	}
}


void
GenerateTerms::positive(Xapian::Document& doc, const std::vector<uint64_t>& accuracy, const std::vector<std::string>& acc_prefix, uint64_t value)
{
	auto it = acc_prefix.begin();
	for (const auto& acc : accuracy) {
		auto term_v = Serialise::positive(value - modulus(value, acc));
		doc.add_term(prefixed(term_v, *it++, ctype_integer));
	}
}


void
GenerateTerms::date(Xapian::Document& doc, const std::vector<uint64_t>& accuracy, const std::vector<std::string>& acc_prefix, const Datetime::tm_t& tm)
{
	auto it = acc_prefix.begin();
	for (const auto& acc : accuracy) {
		switch ((UnitTime)acc) {
			case UnitTime::MILLENNIUM: {
				Datetime::tm_t _tm(GenerateTerms::year(tm.year, 1000));
				doc.add_term(prefixed(Serialise::timestamp(Datetime::timegm(_tm)), *it++, ctype_date));
				break;
			}
			case UnitTime::CENTURY: {
				Datetime::tm_t _tm(GenerateTerms::year(tm.year, 100));
				doc.add_term(prefixed(Serialise::timestamp(Datetime::timegm(_tm)), *it++, ctype_date));
				break;
			}
			case UnitTime::DECADE: {
				Datetime::tm_t _tm(GenerateTerms::year(tm.year, 10));
				doc.add_term(prefixed(Serialise::timestamp(Datetime::timegm(_tm)), *it++, ctype_date));
				break;
			}
			case UnitTime::YEAR: {
				Datetime::tm_t _tm(tm.year);
				doc.add_term(prefixed(Serialise::timestamp(Datetime::timegm(_tm)), *it++, ctype_date));
				break;
			}
			case UnitTime::MONTH: {
				Datetime::tm_t _tm(tm.year, tm.mon);
				doc.add_term(prefixed(Serialise::timestamp(Datetime::timegm(_tm)), *it++, ctype_date));
				break;
			}
			case UnitTime::DAY: {
				Datetime::tm_t _tm(tm.year, tm.mon, tm.day);
				doc.add_term(prefixed(Serialise::timestamp(Datetime::timegm(_tm)), *it++, ctype_date));
				break;
			}
			case UnitTime::HOUR: {
				Datetime::tm_t _tm(tm.year, tm.mon, tm.day, tm.hour);
				doc.add_term(prefixed(Serialise::timestamp(Datetime::timegm(_tm)), *it++, ctype_date));
				break;
			}
			case UnitTime::MINUTE: {
				Datetime::tm_t _tm(tm.year, tm.mon, tm.day, tm.hour, tm.min);
				doc.add_term(prefixed(Serialise::timestamp(Datetime::timegm(_tm)), *it++, ctype_date));
				break;
			}
			case UnitTime::SECOND: {
				Datetime::tm_t _tm(tm.year, tm.mon, tm.day, tm.hour, tm.min, tm.sec);
				doc.add_term(prefixed(Serialise::timestamp(Datetime::timegm(_tm)), *it++, ctype_date));
				break;
			}
		}
	}
}


void
GenerateTerms::geo(Xapian::Document& doc, const std::vector<uint64_t>& accuracy, const std::vector<std::string>& acc_prefix, const RangeList& ranges)
{
	// Index Values and looking for terms generated by accuracy.
	std::unordered_set<std::string> set_terms;
	for (const auto& range : ranges) {
		int idx = -1;
		uint64_t val;
		if (range.start != range.end) {
			std::bitset<SIZE_BITS_ID> b1(range.start), b2(range.end), res;
			for (idx = SIZE_BITS_ID - 1; b1.test(idx) == b2.test(idx); --idx) {
				res.set(idx, b1.test(idx));
			}
			val = res.to_ullong();
		} else {
			val = range.start;
		}
		auto it = acc_prefix.begin();
		for (const auto& acc : accuracy) {
			int pos = START_POS - acc * 2;
			if (idx < pos) {
				uint64_t vterm = val >> pos;
				set_terms.insert(prefixed(Serialise::trixel_id(vterm), *it++, ctype_geo));
			} else {
				break;
			}
		}
	}
	// Insert terms generated by accuracy.
	for (const auto& term : set_terms) {
		doc.add_term(term);
	}
}


void
GenerateTerms::integer(Xapian::Document& doc, const std::vector<uint64_t>& accuracy, const std::vector<std::string>& acc_prefix,
	const std::vector<std::string>& acc_global_prefix, int64_t value)
{
	auto it = acc_prefix.begin();
	auto itg = acc_global_prefix.begin();
	for (const auto& acc : accuracy) {
		auto term_v = Serialise::integer(value - modulus(value, acc));
		doc.add_term(prefixed(term_v, *it++, ctype_integer));
		doc.add_term(prefixed(term_v, *itg++, ctype_integer));
	}
}


void
GenerateTerms::positive(Xapian::Document& doc, const std::vector<uint64_t>& accuracy, const std::vector<std::string>& acc_prefix,
	const std::vector<std::string>& acc_global_prefix, uint64_t value)
{
	auto it = acc_prefix.begin();
	auto itg = acc_global_prefix.begin();
	for (const auto& acc : accuracy) {
		auto term_v = Serialise::positive(value - modulus(value, acc));
		doc.add_term(prefixed(term_v, *it++, ctype_integer));
		doc.add_term(prefixed(term_v, *itg++, ctype_integer));
	}
}


void
GenerateTerms::date(Xapian::Document& doc, const std::vector<uint64_t>& accuracy, const std::vector<std::string>& acc_prefix,
	const std::vector<std::string>& acc_global_prefix, const Datetime::tm_t& tm)
{
	auto it = acc_prefix.begin();
	auto itg = acc_global_prefix.begin();
	for (const auto& acc : accuracy) {
		switch ((UnitTime)acc) {
			case UnitTime::MILLENNIUM: {
				Datetime::tm_t _tm(GenerateTerms::year(tm.year, 1000));
				auto term_v = Serialise::timestamp(Datetime::timegm(_tm));
				doc.add_term(prefixed(term_v, *it++, ctype_date));
				doc.add_term(prefixed(term_v, *itg++, ctype_date));
				break;
			}
			case UnitTime::CENTURY: {
				Datetime::tm_t _tm(GenerateTerms::year(tm.year, 100));
				auto term_v = Serialise::timestamp(Datetime::timegm(_tm));
				doc.add_term(prefixed(term_v, *it++, ctype_date));
				doc.add_term(prefixed(term_v, *itg++, ctype_date));
				break;
			}
			case UnitTime::DECADE: {
				Datetime::tm_t _tm(GenerateTerms::year(tm.year, 10));
				auto term_v = Serialise::timestamp(Datetime::timegm(_tm));
				doc.add_term(prefixed(term_v, *it++, ctype_date));
				doc.add_term(prefixed(term_v, *itg++, ctype_date));
				break;
			}
			case UnitTime::YEAR: {
				Datetime::tm_t _tm(tm.year);
				auto term_v = Serialise::timestamp(Datetime::timegm(_tm));
				doc.add_term(prefixed(term_v, *it++, ctype_date));
				doc.add_term(prefixed(term_v, *itg++, ctype_date));
				break;
			}
			case UnitTime::MONTH: {
				Datetime::tm_t _tm(tm.year, tm.mon);
				auto term_v = Serialise::timestamp(Datetime::timegm(_tm));
				doc.add_term(prefixed(term_v, *it++, ctype_date));
				doc.add_term(prefixed(term_v, *itg++, ctype_date));
				break;
			}
			case UnitTime::DAY: {
				Datetime::tm_t _tm(tm.year, tm.mon, tm.day);
				auto term_v = Serialise::timestamp(Datetime::timegm(_tm));
				doc.add_term(prefixed(term_v, *it++, ctype_date));
				doc.add_term(prefixed(term_v, *itg++, ctype_date));
				break;
			}
			case UnitTime::HOUR: {
				Datetime::tm_t _tm(tm.year, tm.mon, tm.day, tm.hour);
				auto term_v = Serialise::timestamp(Datetime::timegm(_tm));
				doc.add_term(prefixed(term_v, *it++, ctype_date));
				doc.add_term(prefixed(term_v, *itg++, ctype_date));
				break;
			}
			case UnitTime::MINUTE: {
				Datetime::tm_t _tm(tm.year, tm.mon, tm.day, tm.hour, tm.min);
				auto term_v = Serialise::timestamp(Datetime::timegm(_tm));
				doc.add_term(prefixed(term_v, *it++, ctype_date));
				doc.add_term(prefixed(term_v, *itg++, ctype_date));
				break;
			}
			case UnitTime::SECOND: {
				Datetime::tm_t _tm(tm.year, tm.mon, tm.day, tm.hour, tm.min, tm.sec);
				auto term_v = Serialise::timestamp(Datetime::timegm(_tm));
				doc.add_term(prefixed(term_v, *it++, ctype_date));
				doc.add_term(prefixed(term_v, *itg++, ctype_date));
				break;
			}
		}
	}
}


void
GenerateTerms::geo(Xapian::Document& doc, const std::vector<uint64_t>& accuracy, const std::vector<std::string>& acc_prefix,
	const std::vector<std::string>& acc_global_prefix, const RangeList& ranges)
{
	// Index Values and looking for terms generated by accuracy.
	std::unordered_set<std::string> set_terms;
	for (const auto& range : ranges) {
		int idx = -1;
		uint64_t val;
		if (range.start != range.end) {
			std::bitset<SIZE_BITS_ID> b1(range.start), b2(range.end), res;
			for (idx = SIZE_BITS_ID - 1; b1.test(idx) == b2.test(idx); --idx) {
				res.set(idx, b1.test(idx));
			}
			val = res.to_ullong();
		} else {
			val = range.start;
		}
		auto it = acc_prefix.begin();
		auto itg = acc_global_prefix.begin();
		for (const auto& acc : accuracy) {
			int pos = START_POS - acc * 2;
			if (idx < pos) {
				uint64_t vterm = val >> pos;
				auto term_s = Serialise::trixel_id(vterm);
				set_terms.insert(prefixed(term_s, *it++, ctype_geo));
				set_terms.insert(prefixed(term_s, *itg++, ctype_geo));
			} else {
				break;
			}
		}
	}
	// Insert terms generated by accuracy.
	for (const auto& term : set_terms) {
		doc.add_term(term);
	}
}


Xapian::Query
GenerateTerms::date(double start_, double end_, const std::vector<uint64_t>& accuracy, const std::vector<std::string>& acc_prefix, Xapian::termcount wqf)
{
	if (accuracy.empty() || end_ < start_) {
		return Xapian::Query();
	}

	auto tm_s = Datetime::to_tm_t(start_);
	auto tm_e = Datetime::to_tm_t(end_);

	uint64_t diff = tm_e.year - tm_s.year, acc = -1;
	// Find the accuracy needed.
	if (diff) {
		if (diff >= 1000) {
			acc = toUType(UnitTime::MILLENNIUM);
		} else if (diff >= 100) {
			acc = toUType(UnitTime::CENTURY);
		} else if (diff >= 10) {
			acc = toUType(UnitTime::DECADE);
		} else {
			acc = toUType(UnitTime::YEAR);
		}
	} else if (tm_e.mon - tm_s.mon) {
		acc = toUType(UnitTime::MONTH);
	} else if (tm_e.day - tm_s.day) {
		acc = toUType(UnitTime::DAY);
	} else if (tm_e.hour - tm_s.hour) {
		acc = toUType(UnitTime::HOUR);
	} else if (tm_e.min - tm_s.min) {
		acc = toUType(UnitTime::MINUTE);
	} else {
		acc = toUType(UnitTime::SECOND);
	}

	// Find the upper or equal accuracy.
	uint64_t pos = 0, len = accuracy.size();
	while (pos < len && accuracy[pos] <= acc) {
		++pos;
	}

	Xapian::Query query_upper;
	Xapian::Query query_needed;

	// If there is an upper accuracy.
	if (pos < len) {
		auto c_tm_s = tm_s;
		auto c_tm_e = tm_e;
		switch ((UnitTime)accuracy[pos]) {
			case UnitTime::MILLENNIUM:
				query_upper = millennium(c_tm_s, c_tm_e, acc_prefix[pos], wqf);
				break;
			case UnitTime::CENTURY:
				query_upper = century(c_tm_s, c_tm_e, acc_prefix[pos], wqf);
				break;
			case UnitTime::DECADE:
				query_upper = decade(c_tm_s, c_tm_e, acc_prefix[pos], wqf);
				break;
			case UnitTime::YEAR:
				query_upper = year(c_tm_s, c_tm_e, acc_prefix[pos], wqf);
				break;
			case UnitTime::MONTH:
				query_upper = month(c_tm_s, c_tm_e, acc_prefix[pos], wqf);
				break;
			case UnitTime::DAY:
				query_upper = day(c_tm_s, c_tm_e, acc_prefix[pos], wqf);
				break;
			case UnitTime::HOUR:
				query_upper = hour(c_tm_s, c_tm_e, acc_prefix[pos], wqf);
				break;
			case UnitTime::MINUTE:
				query_upper = minute(c_tm_s, c_tm_e, acc_prefix[pos], wqf);
				break;
			case UnitTime::SECOND:
				query_upper = second(c_tm_s, c_tm_e, acc_prefix[pos], wqf);
				break;
		}
	}

	// If there is the needed accuracy.
	if (pos > 0 && acc == accuracy[--pos]) {
		switch ((UnitTime)accuracy[pos]) {
			case UnitTime::MILLENNIUM:
				query_needed = millennium(tm_s, tm_e, acc_prefix[pos], wqf);
				break;
			case UnitTime::CENTURY:
				query_needed = century(tm_s, tm_e, acc_prefix[pos], wqf);
				break;
			case UnitTime::DECADE:
				query_needed = decade(tm_s, tm_e, acc_prefix[pos], wqf);
				break;
			case UnitTime::YEAR:
				query_needed = year(tm_s, tm_e, acc_prefix[pos], wqf);
				break;
			case UnitTime::MONTH:
				query_needed = month(tm_s, tm_e, acc_prefix[pos], wqf);
				break;
			case UnitTime::DAY:
				query_needed = day(tm_s, tm_e, acc_prefix[pos], wqf);
				break;
			case UnitTime::HOUR:
				query_needed = hour(tm_s, tm_e, acc_prefix[pos], wqf);
				break;
			case UnitTime::MINUTE:
				query_needed = minute(tm_s, tm_e, acc_prefix[pos], wqf);
				break;
			case UnitTime::SECOND:
				query_needed = second(tm_s, tm_e, acc_prefix[pos], wqf);
				break;
		}
	}

	if (!query_upper.empty() && !query_needed.empty()) {
		return Xapian::Query(Xapian::Query::OP_AND, query_upper, query_needed);
	} else if (!query_upper.empty()) {
		return query_upper;
	} else {
		return query_needed;
	}
}


Xapian::Query
GenerateTerms::millennium(Datetime::tm_t& tm_s, Datetime::tm_t& tm_e, const std::string& prefix, Xapian::termcount wqf)
{
	Xapian::Query query;

	tm_s.sec = tm_s.min = tm_s.hour = tm_e.sec = tm_e.min = tm_e.hour = 0;
	tm_s.day = tm_s.mon = tm_e.day = tm_e.mon = 1;
	tm_s.year = year(tm_s.year, 1000);
	tm_e.year = year(tm_e.year, 1000);
	size_t num_unions = (tm_e.year - tm_s.year) / 1000;
	if (num_unions < MAX_TERMS) {
		// Reserve upper bound.
		query = Xapian::Query(prefixed(Serialise::serialise(tm_e), prefix, ctype_date), wqf);
		while (tm_s.year != tm_e.year) {
			query = Xapian::Query(Xapian::Query::OP_OR, query, Xapian::Query(prefixed(Serialise::serialise(tm_s), prefix, ctype_date), wqf));
			tm_s.year += 1000;
		}
	}

	return query;
}


Xapian::Query
GenerateTerms::century(Datetime::tm_t& tm_s, Datetime::tm_t& tm_e, const std::string& prefix, Xapian::termcount wqf)
{
	Xapian::Query query;

	tm_s.sec = tm_s.min = tm_s.hour = tm_e.sec = tm_e.min = tm_e.hour = 0;
	tm_s.day = tm_s.mon = tm_e.day = tm_e.mon = 1;
	tm_s.year = year(tm_s.year, 100);
	tm_e.year = year(tm_e.year, 100);
	size_t num_unions = (tm_e.year - tm_s.year) / 100;
	if (num_unions < MAX_TERMS) {
		// Reserve upper bound.
		query = Xapian::Query(prefixed(Serialise::serialise(tm_e), prefix, ctype_date), wqf);
		while (tm_s.year != tm_e.year) {
			query = Xapian::Query(Xapian::Query::OP_OR, query, Xapian::Query(prefixed(Serialise::serialise(tm_s), prefix, ctype_date), wqf));
			tm_s.year += 100;
		}
	}

	return query;
}


Xapian::Query
GenerateTerms::decade(Datetime::tm_t& tm_s, Datetime::tm_t& tm_e, const std::string& prefix, Xapian::termcount wqf)
{
	Xapian::Query query;

	tm_s.sec = tm_s.min = tm_s.hour = tm_e.sec = tm_e.min = tm_e.hour = 0;
	tm_s.day = tm_s.mon = tm_e.day = tm_e.mon = 1;
	tm_s.year = year(tm_s.year, 10);
	tm_e.year = year(tm_e.year, 10);
	size_t num_unions = (tm_e.year - tm_s.year) / 10;
	if (num_unions < MAX_TERMS) {
		// Reserve upper bound.
		query = Xapian::Query(prefixed(Serialise::serialise(tm_e), prefix, ctype_date), wqf);
		while (tm_s.year != tm_e.year) {
			query = Xapian::Query(Xapian::Query::OP_OR, query, Xapian::Query(prefixed(Serialise::serialise(tm_s), prefix, ctype_date), wqf));
			tm_s.year += 10;
		}
	}

	return query;
}


Xapian::Query
GenerateTerms::year(Datetime::tm_t& tm_s, Datetime::tm_t& tm_e, const std::string& prefix, Xapian::termcount wqf)
{
	Xapian::Query query;

	tm_s.sec = tm_s.min = tm_s.hour = tm_e.sec = tm_e.min = tm_e.hour = 0;
	tm_s.day = tm_s.mon = tm_e.day = tm_e.mon = 1;
	size_t num_unions = tm_e.year - tm_s.year;
	if (num_unions < MAX_TERMS) {
		// Reserve upper bound.
		query = Xapian::Query(prefixed(Serialise::serialise(tm_e), prefix, ctype_date), wqf);
		while (tm_s.year != tm_e.year) {
			query = Xapian::Query(Xapian::Query::OP_OR, query, Xapian::Query(prefixed(Serialise::serialise(tm_s), prefix, ctype_date), wqf));
			++tm_s.year;
		}
	}

	return query;
}


Xapian::Query
GenerateTerms::month(Datetime::tm_t& tm_s, Datetime::tm_t& tm_e, const std::string& prefix, Xapian::termcount wqf)
{
	Xapian::Query query;

	tm_s.sec = tm_s.min = tm_s.hour = tm_e.sec = tm_e.min = tm_e.hour = 0;
	tm_s.day = tm_e.day = 1;
	size_t num_unions = tm_e.mon - tm_s.mon;
	if (num_unions < MAX_TERMS) {
		// Reserve upper bound.
		query = Xapian::Query(prefixed(Serialise::serialise(tm_e), prefix, ctype_date), wqf);
		while (tm_s.mon != tm_e.mon) {
			query = Xapian::Query(Xapian::Query::OP_OR, query, Xapian::Query(prefixed(Serialise::serialise(tm_s), prefix, ctype_date), wqf));
			++tm_s.mon;
		}
	}

	return query;
}


Xapian::Query
GenerateTerms::day(Datetime::tm_t& tm_s, Datetime::tm_t& tm_e, const std::string& prefix, Xapian::termcount wqf)
{
	Xapian::Query query;

	tm_s.sec = tm_s.min = tm_s.hour = tm_e.sec = tm_e.min = tm_e.hour = 0;
	size_t num_unions = tm_e.day - tm_s.day;
	if (num_unions < MAX_TERMS) {
		// Reserve upper bound.
		query = Xapian::Query(prefixed(Serialise::serialise(tm_e), prefix, ctype_date), wqf);
		while (tm_s.day != tm_e.day) {
			query = Xapian::Query(Xapian::Query::OP_OR, query, Xapian::Query(prefixed(Serialise::serialise(tm_s), prefix, ctype_date), wqf));
			++tm_s.day;
		}
	}

	return query;
}


Xapian::Query
GenerateTerms::hour(Datetime::tm_t& tm_s, Datetime::tm_t& tm_e, const std::string& prefix, Xapian::termcount wqf)
{
	Xapian::Query query;

	tm_s.sec = tm_s.min = tm_e.sec = tm_e.min = 0;
	size_t num_unions = tm_e.hour - tm_s.hour;
	if (num_unions < MAX_TERMS) {
		// Reserve upper bound.
		query = Xapian::Query(prefixed(Serialise::serialise(tm_e), prefix, ctype_date), wqf);
		while (tm_s.hour != tm_e.hour) {
			query = Xapian::Query(Xapian::Query::OP_OR, query, Xapian::Query(prefixed(Serialise::serialise(tm_s), prefix, ctype_date), wqf));
			++tm_s.hour;
		}
	}

	return query;
}


Xapian::Query
GenerateTerms::minute(Datetime::tm_t& tm_s, Datetime::tm_t& tm_e, const std::string& prefix, Xapian::termcount wqf)
{
	Xapian::Query query;

	tm_s.sec = tm_e.sec = 0;
	size_t num_unions = tm_e.min - tm_s.min;
	if (num_unions < MAX_TERMS) {
		// Reserve upper bound.
		query = Xapian::Query(prefixed(Serialise::serialise(tm_e), prefix, ctype_date), wqf);
		while (tm_s.min != tm_e.min) {
			query = Xapian::Query(Xapian::Query::OP_OR, query, Xapian::Query(prefixed(Serialise::serialise(tm_s), prefix, ctype_date), wqf));
			++tm_s.min;
		}
	}

	return query;
}


Xapian::Query
GenerateTerms::second(Datetime::tm_t& tm_s, Datetime::tm_t& tm_e, const std::string& prefix, Xapian::termcount wqf)
{
	Xapian::Query query;

	size_t num_unions = tm_e.sec - tm_s.sec;
	if (num_unions < MAX_TERMS) {
		query = Xapian::Query(prefixed(Serialise::serialise(tm_e), prefix, ctype_date), wqf);
		while (tm_s.sec != tm_e.sec) {
			query = Xapian::Query(Xapian::Query::OP_OR, query, Xapian::Query(prefixed(Serialise::serialise(tm_s), prefix, ctype_date), wqf));
			++tm_s.sec;
		}
	}

	return query;
}


Xapian::Query
GenerateTerms::geo(const std::vector<range_t>& ranges, const std::vector<uint64_t>& accuracy, const std::vector<std::string>& acc_prefix, Xapian::termcount wqf)
{
	// The user does not specify the accuracy.
	if (acc_prefix.empty() || ranges.empty()) {
		return Xapian::Query();
	}

	std::vector<int> pos_accuracy;
	pos_accuracy.reserve(accuracy.size());
	for (const auto& acc : accuracy) {
		pos_accuracy.push_back(START_POS - acc * 2);
	}

	std::map<uint64_t, std::string> results;
	for (const auto& range : ranges) {
		std::bitset<SIZE_BITS_ID> b1(range.start), b2(range.end), res;
		auto idx = -1;
		uint64_t val;
		if (range.start != range.end) {
			for (idx = SIZE_BITS_ID - 1; idx > 0 && b1.test(idx) == b2.test(idx); --idx) {
				res.set(idx, b1.test(idx));
			}
			val = res.to_ullong();
		} else {
			val = range.start;
		}

		for (int i = accuracy.size() - 1; i >= 0; --i) {
			if (pos_accuracy[i] > idx) {
				results.insert(std::make_pair(val >> pos_accuracy[i], acc_prefix[i]));
				break;
			}
		}
	}

	// The search have trixels more big that the biggest trixel in accuracy.
	if (results.empty()) {
		return Xapian::Query();
	}

	// Delete duplicates terms.
	auto it = results.begin();
	auto last_valid(std::bitset<SIZE_BITS_ID>(it->first).to_string());
	last_valid.assign(last_valid.substr(last_valid.find('1')));

	Xapian::Query query;
	const auto it_e = results.end();
	for (++it; it != it_e; ++it) {
		if (isnotSubtrixel(last_valid, it->first)) {
			Xapian::Query query_(prefixed(Serialise::serialise(it->first), it->second, ctype_geo), wqf);
			if (query.empty()) {
				query = query_;
			} else {
				query = Xapian::Query(Xapian::Query::OP_OR, query, query_);
			}
		}
	}

	return query;
}
