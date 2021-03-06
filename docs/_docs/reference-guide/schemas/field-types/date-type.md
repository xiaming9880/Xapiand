---
title: Date / Time Datatypes
short_title: Date / Time
---

# Datetime Datatype

JSON doesn't have a date, datetime, or time datatype, so dates in Xapiand can
either be:

* Strings containing formatted dates, e.g. `"2001-05-24T10:41:25.123Z"`,
  `"2001-05-24"`, `"2001/05/24 10:41:25.123"` or ISO-8601.
* A floating point number representing seconds-since-the-epoch.
* An object containing a `_datetime` or `_date` type.

Internally, dates are converted to UTC (if the time-zone is specified) and
stored as a floating point number representing seconds-since-the-epoch.

Queries on dates are internally converted to range queries on this
representation, and the result of aggregations and stored fields is converted
back to a string depending on the date format that is associated with the field.

{% capture req %}

```json
UPDATE /bank/1

{
  "birthday": "2001-05-24T10:41:25.123Z"
}
```
{% endcapture %}
{% include curl.html req=req %}


{% capture req %}

```json
UPDATE /bank/1

{
  "birthday": {
    "_datetime": {
      "_year": 2001,
      "_month": 5,
      "_day": 24,
      "_hour": 10,
      "_min": 41,
      "_sec": 25,
      "_fsec": 0.123
    }
  }
}
```
{% endcapture %}
{% include curl.html req=req %}


## Optimized Range Searches

Data types like date and numeric are often used for range search. Due to the way
range searches are implemented, searchig for values is not as performant as
seaching by terms. To improve the performance of the search, Xapiand uses the
`_accuracy` keyword, which indexes terms for value thresholds which later are
used during the querying to improve the filtering and searching.

In the above example terms for the day, month and year are generated.

Default accuracy in datetime fields is:

```json
[ "hour", "day", "month", "year", "decade", "century" ]
```


## Date Math Expressions

The _Date Datatype_ supports using date math expressions when using it in a
query/filter or during indexation. Whenever durations need to be specified,
e.g. for a timeout parameter, the duration can be specified.

The expression starts with an "_anchor_" date, which can be either `now` or a
date string (in the applicable format) ending with `||`. This anchor date can
optionally be followed by one or more maths expressions:

* `+1h` - add one hour
* `-1d` - subtract one day
* `/d` - round down to the nearest day

The supported units are:

|-----|---------|
| `y` | Years   |
| `M` | Months  |
| `w` | Weeks   |
| `d` | Days    |
| `h` | Hours   |
| `m` | Minutes |
| `s` | Seconds |


### Example

{% capture req %}

```json
UPDATE /bank/2

{
    "birthday": {
      "_value": "2019-01-01||-18y+4M+23d",
      "_type": "datetime"
    }
}
```
{% endcapture %}
{% include curl.html req=req %}

The above example is indexed as "`2001-05-24`".


## Parameters

The following parameters are accepted by _Date_ fields:

|---------------------------------------|-----------------------------------------------------------------------------------------|
| `_accuracy`                           | Array with the accuracies to be indexed: `"second"`, `"minute"`, `"day"`, `"hour"`, `"month"`, `"year"`, `"decade"`, `"century"`, `"millennium"` |
| `_value`                              | The value for the field. (Only used at index time).                                     |
| `_index`                              | The mode the field will be indexed as: `"none"`, `"field_terms"`, `"field_values"`, `"field_all"`, `"field"`, `"global_terms"`, `"global_values"`, `"global_all"`, `"global"`, `"terms"`, `"values"`, `"all"`. (The default is `"field_all"`). |
| `_slot`                               | The slot number. (It's calculated by default).                                          |
| `_prefix`                             | The prefix the term is going to be indexed with. (It's calculated by default)           |
| `_weight`                             | The weight the term is going to be indexed with.                                        |


# Time Datatype

The `time` can also be a type without the entire date:

For example:
{% capture req %}

```json
UPDATE /bank/1

{
  "wakeupTime": {
    "_value": "10:12:12.123",
    "_type": "time"
  }
}
```
{% endcapture %}
{% include curl.html req=req %}



# Time Delta Datatype

For example:
{% capture req %}

```json
UPDATE /bank/1

{
  "delay": {
    "_type": "timedelta",
    "_value": "+10:12:12.123"
  }
}
```
{% endcapture %}
{% include curl.html req=req %}
