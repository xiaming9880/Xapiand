/*
 * Copyright (C) 2015-2019 Dubalu LLC. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#pragma once

#include <algorithm>                // for std::nth_element, std::max_element
#include <cmath>                    // for sqrt
#include <cstdio>
#include <cstring>                  // for size_t
#include <limits>                   // for std::numeric_limits
#include <map>                      // for std::map
#include <memory>                   // for std::shared_ptr
#include <string>                   // for std::string
#include "string_view.hh"           // for std::string_view
#include <utility>                  // for std::pair
#include <vector>                   // for std::vector
#include <xapian.h>                 // for valueno

#include "aggregation.h"            // for BaseAggregation, AGGREGATION_*
#include "exception.h"              // for AggregationError, MSG_AggregationError
#include "msgpack.h"                // for MsgPack, object::object
#include "serialise_list.h"         // for StringList, RangeList


class Schema;


template <typename Handler>
class HandledSubAggregation;


class ValuesHandler {
	using func_value_handle = void (HandledSubAggregation<ValuesHandler>::*)(const Xapian::Document&);

protected:
	FieldType _type;
	Xapian::valueno _slot;
	func_value_handle _func;

public:
	ValuesHandler(const MsgPack& conf, const std::shared_ptr<Schema>& schema);

	std::vector<std::string> values(const Xapian::Document& doc) const;

	FieldType get_type() {
		return _type;
	}

	void operator()(HandledSubAggregation<ValuesHandler>* agg, const Xapian::Document& doc) const {
		(agg->*_func)(doc);
	}
};


class TermsHandler {
	using func_value_handle = void (HandledSubAggregation<TermsHandler>::*)(const Xapian::Document&);

protected:
	FieldType _type;
	std::string _prefix;
	func_value_handle _func;

public:
	TermsHandler(const MsgPack& conf, const std::shared_ptr<Schema>& schema);

	std::vector<std::string> values(const Xapian::Document& doc) const;

	FieldType get_type() {
		return _type;
	}

	void operator()(HandledSubAggregation<TermsHandler>* agg, const Xapian::Document& doc) const {
		(agg->*_func)(doc);
	}
};


template <typename Handler>
class HandledSubAggregation : public BaseAggregation {
protected:
	Handler _handler;
	const MsgPack& _conf;

public:
	HandledSubAggregation(const MsgPack& conf, const std::shared_ptr<Schema>& schema)
		: _handler(conf, schema),
		  _conf(conf) { }

	HandledSubAggregation(const MsgPack& context, std::string_view name, const std::shared_ptr<Schema>& schema)
		: HandledSubAggregation(context.at(name), schema) { }

	virtual void aggregate_float(long double, const Xapian::Document&) {
		THROW(AggregationError, "float type is not supported");
	}

	virtual void aggregate_integer(int64_t, const Xapian::Document&) {
		THROW(AggregationError, "integer type is not supported");
	}

	virtual void aggregate_positive(uint64_t, const Xapian::Document&) {
		THROW(AggregationError, "positive type is not supported");
	}

	virtual void aggregate_date(double, const Xapian::Document&) {
		THROW(AggregationError, "date type is not supported");
	}

	virtual void aggregate_time(double, const Xapian::Document&) {
		THROW(AggregationError, "time type is not supported");
	}

	virtual void aggregate_timedelta(double, const Xapian::Document&) {
		THROW(AggregationError, "timedelta type is not supported");
	}

	virtual void aggregate_boolean(bool, const Xapian::Document&) {
		THROW(AggregationError, "boolean type is not supported");
	}

	virtual void aggregate_string(std::string_view, const Xapian::Document&) {
		THROW(AggregationError, "string type is not supported");
	}

	virtual void aggregate_geo(const range_t&, const Xapian::Document&) {
		THROW(AggregationError, "geo type is not supported");
	}

	virtual void aggregate_uuid(std::string_view, const Xapian::Document&) {
		THROW(AggregationError, "uuid type is not supported");
	}

	void _aggregate_float(const Xapian::Document& doc) {
		for (const auto& value : _handler.values(doc)) {
			aggregate_float(Unserialise::_float(value), doc);
		}
	}

	void _aggregate_integer(const Xapian::Document& doc) {
		for (const auto& value : _handler.values(doc)) {
			aggregate_integer(Unserialise::integer(value), doc);
		}
	}

	void _aggregate_positive(const Xapian::Document& doc) {
		for (const auto& value : _handler.values(doc)) {
			aggregate_positive(Unserialise::positive(value), doc);
		}
	}

	void _aggregate_date(const Xapian::Document& doc) {
		for (const auto& value : _handler.values(doc)) {
			aggregate_date(Unserialise::timestamp(value), doc);
		}
	}

	void _aggregate_time(const Xapian::Document& doc) {
		for (const auto& value : _handler.values(doc)) {
			aggregate_time(Unserialise::time_d(value), doc);
		}
	}

	void _aggregate_timedelta(const Xapian::Document& doc) {
		for (const auto& value : _handler.values(doc)) {
			aggregate_timedelta(Unserialise::timedelta_d(value), doc);
		}
	}

	void _aggregate_boolean(const Xapian::Document& doc)  {
		for (const auto& value : _handler.values(doc)) {
			aggregate_boolean(Unserialise::boolean(value), doc);
		}
	}

	void _aggregate_string(const Xapian::Document& doc) {
		for (const auto& value : _handler.values(doc)) {
			aggregate_string(value, doc);
		}
	}

	void _aggregate_geo(const Xapian::Document& doc) {
		for (const auto& value : _handler.values(doc)) {
			for (const auto& range : RangeList(value)) {
				aggregate_geo(range, doc);
			}
		}
	}

	void _aggregate_uuid(const Xapian::Document& doc) {
		for (const auto& value : _handler.values(doc)) {
			aggregate_uuid(Unserialise::uuid(value), doc);
		}
	}

	void operator()(const Xapian::Document& doc) override {
		_handler(this, doc);
	}
};


class MetricCount : public HandledSubAggregation<ValuesHandler> {
protected:
	size_t _count;

public:
	MetricCount(const MsgPack& context, std::string_view name, const std::shared_ptr<Schema>& schema)
		: HandledSubAggregation<ValuesHandler>(context, name, schema),
		  _count(0) { }

	MsgPack get_result() override {
		return {
			{ AGGREGATION_COUNT, _count },
		};
	}

	void _aggregate() {
		++_count;
	}

	void aggregate_float(long double, const Xapian::Document&) override {
		_aggregate();
	}

	void aggregate_integer(int64_t, const Xapian::Document&) override {
		_aggregate();
	}

	void aggregate_positive(uint64_t, const Xapian::Document&) override {
		_aggregate();
	}

	void aggregate_date(double, const Xapian::Document&) override {
		_aggregate();
	}

	void aggregate_time(double, const Xapian::Document&) override {
		_aggregate();
	}

	void aggregate_timedelta(double, const Xapian::Document&) override {
		_aggregate();
	}

	void aggregate_boolean(bool, const Xapian::Document&) override {
		_aggregate();
	}

	void aggregate_string(std::string_view, const Xapian::Document&) override {
		_aggregate();
	}

	void aggregate_geo(const range_t&, const Xapian::Document&) override {
		_aggregate();
	}

	void aggregate_uuid(std::string_view, const Xapian::Document&) override {
		_aggregate();
	}
};


class MetricSum : public HandledSubAggregation<ValuesHandler> {
protected:
	long double _sum;

public:
	MetricSum(const MsgPack& context, std::string_view name, const std::shared_ptr<Schema>& schema)
		: HandledSubAggregation<ValuesHandler>(context, name, schema),
		  _sum(0) { }

	MsgPack get_result() override {
		return {
			{ AGGREGATION_SUM, static_cast<double>(_sum) },
		};
	}

	void _aggregate(long double value) {
		_sum += value;
	}

	void aggregate_float(long double value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_integer(int64_t value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_positive(uint64_t value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_date(double value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_time(double value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_timedelta(double value, const Xapian::Document&) override {
		_aggregate(value);
	}
};


class MetricAvg : public MetricSum {
protected:
	size_t _count;

	long double _avg;

public:
	MetricAvg(const MsgPack& context, std::string_view name, const std::shared_ptr<Schema>& schema)
		: MetricSum(context, name, schema),
		  _count(0),
		  _avg(0) { }

	MsgPack get_result() override {
		return {
			{ AGGREGATION_AVG, static_cast<double>(_avg) },
		};
	}

	void _aggregate(long double value) {
		++_count;
		_sum += value;
	}

	void aggregate_float(long double value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_integer(int64_t value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_positive(uint64_t value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_date(double value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_time(double value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_timedelta(double value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void update() override {
		_avg = _sum ? _sum / _count : _sum;
	}
};


class MetricExtendedStats;
class MetricStats;


class MetricMin : public HandledSubAggregation<ValuesHandler> {
	friend class MetricStats;
	friend class MetricExtendedStats;

protected:
	long double _min;

public:
	MetricMin(const MsgPack& conf, const std::shared_ptr<Schema>& schema)
		: HandledSubAggregation<ValuesHandler>(conf, schema),
		  _min(std::numeric_limits<long double>::max()) { }

	MetricMin(const MsgPack& context, std::string_view name, const std::shared_ptr<Schema>& schema)
		: HandledSubAggregation<ValuesHandler>(context, name, schema),
		  _min(std::numeric_limits<long double>::max()) { }

	MsgPack get_result() override {
		return {
			{ AGGREGATION_MIN, static_cast<double>(_min) },
		};
	}

	void _aggregate(long double value) {
		if (value < _min) {
			_min = value;
		}
	}

	void aggregate_float(long double value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_integer(int64_t value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_positive(uint64_t value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_date(double value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_time(double value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_timedelta(double value, const Xapian::Document&) override {
		_aggregate(value);
	}
};


class MetricMax : public HandledSubAggregation<ValuesHandler> {
	friend class MetricStats;
	friend class MetricExtendedStats;

protected:
	long double _max;

public:
	MetricMax(const MsgPack& conf, const std::shared_ptr<Schema>& schema)
		: HandledSubAggregation<ValuesHandler>(conf, schema),
		  _max(std::numeric_limits<long double>::min()) { }

	MetricMax(const MsgPack& context, std::string_view name, const std::shared_ptr<Schema>& schema)
		: HandledSubAggregation<ValuesHandler>(context, name, schema),
		  _max(std::numeric_limits<long double>::min()) { }

	MsgPack get_result() override {
		return {
			{ AGGREGATION_MAX, static_cast<double>(_max) },
		};
	}

	void _aggregate(long double value) {
		if (value > _max) {
			_max = value;
		}
	}

	void aggregate_float(long double value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_integer(int64_t value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_positive(uint64_t value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_date(double value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_time(double value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_timedelta(double value, const Xapian::Document&) override {
		_aggregate(value);
	}
};


class MetricVariance : public MetricAvg {
protected:
	long double _sq_sum;
	long double _variance;

public:
	MetricVariance(const MsgPack& context, std::string_view name, const std::shared_ptr<Schema>& schema)
		: MetricAvg(context, name, schema),
		  _sq_sum(0),
		  _variance(0) { }

	MsgPack get_result() override {
		return {
			{ AGGREGATION_VARIANCE, static_cast<double>(_variance) },
		};
	}

	void _aggregate(long double value) {
		++_count;
		_sum += value;
		_sq_sum += value * value;
	}

	void aggregate_float(long double value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_integer(int64_t value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_positive(uint64_t value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_date(double value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_time(double value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_timedelta(double value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void update() override {
		MetricAvg::update();
		_variance = (_sq_sum - _count * _avg * _avg) / (_count - 1);
	}
};


// Standard deviation.
class MetricStdDeviation : public MetricVariance {
protected:
	long double _sigma;

	long double _std;

public:
	MetricStdDeviation(const MsgPack& context, std::string_view name, const std::shared_ptr<Schema>& schema)
		: MetricVariance(context, name, schema),
		  _sigma{2.0} {
		const auto it = _conf.find(AGGREGATION_SIGMA);
		if (it != _conf.end()) {
			const auto& sigma_value = it.value();
			switch (sigma_value.getType()) {
				case MsgPack::Type::POSITIVE_INTEGER:
				case MsgPack::Type::NEGATIVE_INTEGER:
				case MsgPack::Type::FLOAT:
					_sigma = sigma_value.as_f64();
					if (_sigma >= 0) {
						break;
					}
				default:
					THROW(AggregationError, "'%s' must be a positive number", AGGREGATION_SIGMA);
			}
		}
	}

	MsgPack get_result() override {
		return {
			{ AGGREGATION_STD, static_cast<double>(_std) },
			{ AGGREGATION_STD_BOUNDS, {
				{ AGGREGATION_UPPER, static_cast<double>(_avg + _std * _sigma) },
				{ AGGREGATION_LOWER, static_cast<double>(_avg - _std * _sigma) },
			}},
		};
	}

	void update() override {
		MetricVariance::update();
		_std = std::sqrt(_variance);
	}
};


class MetricMedian : public HandledSubAggregation<ValuesHandler> {
	std::vector<long double> values;

public:
	MetricMedian(const MsgPack& context, std::string_view name, const std::shared_ptr<Schema>& schema)
		: HandledSubAggregation<ValuesHandler>(context, name, schema) { }

	MsgPack get_result() override {
		double _median = 0.0;
		if (!values.empty()) {
			size_t median_pos = values.size();
			if (median_pos % 2 == 0) {
				median_pos /= 2;
				std::nth_element(values.begin(), values.begin() + median_pos, values.end());
				auto val1 = values[median_pos];
				std::nth_element(values.begin(), values.begin() + median_pos - 1, values.end());
				_median = static_cast<double>((val1 + values[median_pos - 1]) / 2);
			} else {
				median_pos /= 2;
				std::nth_element(values.begin(), values.begin() + median_pos, values.end());
				_median = static_cast<double>(values[median_pos]);
			}
		}
		return {
			{ AGGREGATION_MEDIAN, _median },
		};
	}

	void _aggregate(long double value) {
		values.push_back(value);
	}

	void aggregate_float(long double value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_integer(int64_t value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_positive(uint64_t value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_date(double value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_time(double value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_timedelta(double value, const Xapian::Document&) override {
		_aggregate(value);
	}
};


class MetricMode : public HandledSubAggregation<ValuesHandler> {
	std::map<long double, size_t> _histogram;

public:
	MetricMode(const MsgPack& context, std::string_view name, const std::shared_ptr<Schema>& schema)
		: HandledSubAggregation<ValuesHandler>(context, name, schema) { }

	MsgPack get_result() override {
		double _mode = 0.0;
		if (!_histogram.empty()) {
			auto it = std::max_element(_histogram.begin(), _histogram.end(), [](const std::pair<double, size_t>& a, const std::pair<double, size_t>& b) { return a.second < b.second; });
			_mode = static_cast<double>(it->first);
		}
		return {
			{ AGGREGATION_MODE, _mode },
		};
	}

	void _aggregate(long double value) {
		++_histogram[value];
	}

	void aggregate_float(long double value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_integer(int64_t value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_positive(uint64_t value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_date(double value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_time(double value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_timedelta(double value, const Xapian::Document&) override {
		_aggregate(value);
	}
};


class MetricStats : public MetricAvg {
protected:
	MetricMin _min_metric;
	MetricMax _max_metric;

public:
	MetricStats(const MsgPack& context, std::string_view name, const std::shared_ptr<Schema>& schema)
		: MetricAvg(context, name, schema),
		  _min_metric(_conf, schema),
		  _max_metric(_conf, schema) { }

	MsgPack get_result() override {
		return {
			{ AGGREGATION_COUNT, _count },
			{ AGGREGATION_MIN, static_cast<double>(_min_metric._min) },
			{ AGGREGATION_MAX, static_cast<double>(_max_metric._max) },
			{ AGGREGATION_AVG, static_cast<double>(_avg) },
			{ AGGREGATION_SUM, static_cast<double>(_sum) },
		};
	}

	void _aggregate(long double value) {
		_min_metric._aggregate(value);
		_max_metric._aggregate(value);
		MetricAvg::_aggregate(value);
	}

	void aggregate_float(long double value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_integer(int64_t value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_positive(uint64_t value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_date(double value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_time(double value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_timedelta(double value, const Xapian::Document&) override {
		_aggregate(value);
	}
};


class MetricExtendedStats : public MetricStdDeviation {
protected:
	MetricMin _min_metric;
	MetricMax _max_metric;

public:
	MetricExtendedStats(const MsgPack& context, std::string_view name, const std::shared_ptr<Schema>& schema)
		: MetricStdDeviation(context, name, schema),
		  _min_metric(_conf, schema),
		  _max_metric(_conf, schema) { }

	MsgPack get_result() override {
		return {
			{ AGGREGATION_COUNT, _count },
			{ AGGREGATION_MIN, static_cast<double>(_min_metric._min) },
			{ AGGREGATION_MAX, static_cast<double>(_max_metric._max) },
			{ AGGREGATION_AVG, static_cast<double>(_avg) },
			{ AGGREGATION_SUM, static_cast<double>(_sum) },
			{ AGGREGATION_SUM_OF_SQ, static_cast<double>(_sq_sum) },
			{ AGGREGATION_VARIANCE, static_cast<double>(_variance) },
			{ AGGREGATION_STD, static_cast<double>(_std) },
			{ AGGREGATION_STD_BOUNDS, {
				{ AGGREGATION_UPPER, static_cast<double>(_avg + _std * _sigma) },
				{ AGGREGATION_LOWER, static_cast<double>(_avg - _std * _sigma) },
			}},
		};
	}

	void _aggregate(long double value) {
		_min_metric._aggregate(value);
		_max_metric._aggregate(value);
		MetricStdDeviation::_aggregate(value);
	}

	void aggregate_float(long double value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_integer(int64_t value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_positive(uint64_t value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_date(double value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_time(double value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_timedelta(double value, const Xapian::Document&) override {
		_aggregate(value);
	}
};
