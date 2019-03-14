/*
 * Copyright (c) 2015-2019 Dubalu LLC
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

#include "database/handler.h"

#include <algorithm>                        // for min, move
#include <array>                            // for std::array
#include <cctype>                           // for tolower
#include <exception>                        // for std::exception
#include <utility>                          // for std::move

#include "cast.h"                           // for Cast
#include "chaipp/exception.h"               // for chaipp::Error
#include "database/database.h"              // for Database
#include "database/lock.h"                  // for lock_database
#include "database/shard.h"                 // for Shard
#include "database/utils.h"                 // for split_path_id
#include "database/wal.h"                   // for DatabaseWAL
#include "exception.h"                      // for ClientError
#include "length.h"                         // for serialise_string, unserialise_string
#include "log.h"                            // for L_CALL
#include "manager.h"                        // for XapiandManager
#include "msgpack.h"                        // for MsgPack
#include "msgpack_patcher.h"                // for apply_patch
#include "aggregations/aggregations.h"      // for AggregationMatchSpy
#include "multivalue/keymaker.h"            // for Multi_MultiValueKeyMaker
#include "opts.h"                           // for opts::
#include "query_dsl.h"                      // for QueryDSL
#include "rapidjson/document.h"             // for Document
#include "repr.hh"                          // for repr
#include "reserved/query_dsl.h"             // for RESERVED_QUERYDSL_*
#include "reserved/schema.h"                // for RESERVED_*
#include "response.h"                       // for RESPONSE_*
#include "schema.h"                         // for Schema, required_spc_t
#include "schemas_lru.h"                    // for SchemasLRU
#include "script.h"                         // for Script
#include "serialise.h"                      // for cast, serialise, type
#include "server/http_utils.h"              // for catch_http_errors

#ifdef XAPIAND_CHAISCRIPT
#include "chaipp/chaipp.h"                  // for chaipp namespace
#endif


// #undef L_DEBUG
// #define L_DEBUG L_GREY
// #undef L_CALL
// #define L_CALL L_STACKED_DIM_GREY


constexpr int CONFLICT_RETRIES = 10;   // Number of tries for resolving version conflicts

constexpr size_t NON_STORED_SIZE_LIMIT = 1024 * 1024;

const std::string dump_metadata_header ("xapiand-dump-meta");
const std::string dump_schema_header("xapiand-dump-schm");
const std::string dump_documents_header("xapiand-dump-docs");


Xapian::docid
to_docid(std::string_view document_id)
{
	size_t sz = document_id.size();
	if (sz > 2 && document_id[0] == ':' && document_id[1] == ':') {
		document_id.remove_prefix(2);
		try {
			return static_cast<Xapian::docid>(strict_stoull(document_id));
		} catch (const InvalidArgument& er) {
			THROW(ClientError, "Value {} cannot be cast to integer [{}]", repr(document_id), er.what());
		} catch (const OutOfRange& er) {
			THROW(ClientError, "Value {} cannot be cast to integer [{}]", repr(document_id), er.what());
		}
	}
	return static_cast<Xapian::docid>(0);
}


static void
inject_blob(Data& data, const MsgPack& obj)
{
	auto blob_it = obj.find(RESERVED_BLOB);
	if (blob_it == obj.end()) {
		THROW(ClientError, "Data inconsistency, objects in '{}' must contain '{}'", RESERVED_DATA, RESERVED_BLOB);
	}
	auto& blob_value = blob_it.value();
	if (!blob_value.is_string()) {
		THROW(ClientError, "Data inconsistency, '{}' must be a string", RESERVED_BLOB);
	}

	auto content_type_it = obj.find(RESERVED_CONTENT_TYPE);
	if (content_type_it == obj.end()) {
		THROW(ClientError, "Data inconsistency, objects in '{}' must contain '{}'", RESERVED_DATA, RESERVED_CONTENT_TYPE);
	}
	auto& content_type_value = content_type_it.value();
	auto ct_type = ct_type_t(content_type_value.is_string() ? content_type_value.str_view() : "");
	if (ct_type.empty()) {
		THROW(ClientError, "Data inconsistency, '{}' must be a valid content type string", RESERVED_CONTENT_TYPE);
	}

	std::string_view type;
	auto type_it = obj.find(RESERVED_TYPE);
	if (type_it == obj.end()) {
		type = "inplace";
	} else {
		auto& type_value = type_it.value();
		if (!type_value.is_string()) {
			THROW(ClientError, "Data inconsistency, '{}' must be either \"inplace\" or \"stored\"", RESERVED_TYPE);
		}
		type = type_value.str_view();
	}

	if (type == "inplace") {
		auto blob = blob_value.str_view();
		if (blob.size() > NON_STORED_SIZE_LIMIT) {
			THROW(ClientError, "Non-stored object has a size limit of {}", string::from_bytes(NON_STORED_SIZE_LIMIT));
		}
		data.update(ct_type, blob);
	} else if (type == "stored") {
		data.update(ct_type, -1, 0, 0, blob_value.str_view());
	} else {
		THROW(ClientError, "Data inconsistency, '{}' must be either \"inplace\" or \"stored\"", RESERVED_TYPE);
	}
}


static void
inject_data(Data& data, const MsgPack& obj)
{
	auto data_it = obj.find(RESERVED_DATA);
	if (data_it != obj.end()) {
		auto& _data = data_it.value();
		switch (_data.getType()) {
			case MsgPack::Type::STR: {
				auto blob = _data.str_view();
				if (blob.size() > NON_STORED_SIZE_LIMIT) {
					THROW(ClientError, "Non-stored object has a size limit of {}", string::from_bytes(NON_STORED_SIZE_LIMIT));
				}
				data.update("application/octet-stream", blob);
				break;
			}
			case MsgPack::Type::NIL:
			case MsgPack::Type::UNDEFINED:
				data.erase("application/octet-stream");
				break;
			case MsgPack::Type::MAP:
				inject_blob(data, _data);
				break;
			case MsgPack::Type::ARRAY:
				for (auto& blob : _data) {
					inject_blob(data, blob);
				}
				break;
			default:
				THROW(ClientError, "Data inconsistency, '{}' must be an array or an object", RESERVED_DATA);
		}
	}
}


class FilterPrefixesExpandDecider : public Xapian::ExpandDecider {
	std::vector<std::string> prefixes;

public:
	FilterPrefixesExpandDecider(std::vector<std::string>  prefixes_)
		: prefixes(std::move(prefixes_)) { }

	bool operator() (const std::string& term) const override {
		for (const auto& prefix : prefixes) {
			if (string::startswith(term, prefix)) {
				return true;
			}
		}

		return prefixes.empty();
	}
};


//  ____        _        _                    _   _                 _ _
// |  _ \  __ _| |_ __ _| |__   __ _ ___  ___| | | | __ _ _ __   __| | | ___ _ __
// | | | |/ _` | __/ _` | '_ \ / _` / __|/ _ \ |_| |/ _` | '_ \ / _` | |/ _ \ '__|
// | |_| | (_| | || (_| | |_) | (_| \__ \  __/  _  | (_| | | | | (_| | |  __/ |
// |____/ \__,_|\__\__,_|_.__/ \__,_|___/\___|_| |_|\__,_|_| |_|\__,_|_|\___|_|
//

DatabaseHandler::DatabaseHandler()
	: flags(0),
	  method(HTTP_GET) { }


DatabaseHandler::DatabaseHandler(const Endpoints& endpoints_, int flags_, enum http_method method_, std::shared_ptr<std::unordered_set<std::string>> context_)
	: flags(flags_),
	  endpoints(endpoints_),
	  method(method_),
	  context(std::move(context_)) { }


std::shared_ptr<Schema>
DatabaseHandler::get_schema(const MsgPack* obj)
{
	L_CALL("DatabaseHandler::get_schema(<obj>)");
	auto s = XapiandManager::schemas()->get(this, obj, (obj != nullptr) && ((flags & DB_WRITABLE) != 0));
	return std::make_shared<Schema>(std::move(std::get<0>(s)), std::move(std::get<1>(s)), std::move(std::get<2>(s)));
}


void
DatabaseHandler::reset(const Endpoints& endpoints_, int flags_, enum http_method method_, const std::shared_ptr<std::unordered_set<std::string>>& context_)
{
	L_CALL("DatabaseHandler::reset({}, {:#x}, <method>)", repr(endpoints_.to_string()), flags_);

	if (endpoints_.empty()) {
		THROW(ClientError, "It is expected at least one endpoint");
	}

	method = method_;

	if (endpoints != endpoints_ || flags != flags_) {
		endpoints = endpoints_;
		flags = flags_;
	}

	context = context_;
}


#if XAPIAND_DATABASE_WAL
MsgPack
DatabaseHandler::repr_wal(uint32_t start_revision, uint32_t end_revision, bool unserialised)
{
	L_CALL("DatabaseHandler::repr_wal({}, {})", start_revision, end_revision);

	if (endpoints.size() != 1) {
		THROW(ClientError, "It is expected one single endpoint");
	}

	// WAL required on a local writable database, open it.
	DatabaseWAL wal(endpoints[0].path);
	return wal.repr(start_revision, end_revision, unserialised);
}
#endif


MsgPack
DatabaseHandler::check()
{
	L_CALL("DatabaseHandler::check()");

	if (endpoints.size() != 1) {
		THROW(ClientError, "It is expected one single endpoint");
	}

	try {
		return {
			{"errors", Xapian::Database::check(endpoints[0].path)},
		};
	} catch (const Xapian::Error &error) {
		return {
			{"error", error.get_description()},
		};
	} catch (...) {
		return {
			{"error", "Unknown error"},
		};
	}
}


Document
DatabaseHandler::get_document_term(const std::string& term_id)
{
	L_CALL("DatabaseHandler::get_document_term({})", repr(term_id));

	lock_database lk_db(endpoints, flags);
	auto did = lk_db->find_document(term_id);
	return Document(did, this);
}


std::unique_ptr<MsgPack>
DatabaseHandler::call_script(const MsgPack& object, const std::string& term_id, const Script& script, const Data& data)
{
#ifdef XAPIAND_CHAISCRIPT
	auto processor = chaipp::Processor::compile(script);
	if (processor) {
		std::string http_method;
		switch (method) {
			case HTTP_PUT:
				http_method = "PUT";
				break;
			case HTTP_PATCH:
				http_method = "PATCH";
				break;
			case HTTP_MERGE:  // TODO: Remove MERGE (method was renamed to UPDATE)
			case HTTP_UPDATE:
				http_method = "UPDATE";
				break;
			case HTTP_DELETE:
				http_method = "DELETE";
				break;
			case HTTP_GET:
				http_method = "GET";
				break;
			case HTTP_POST:
				http_method = "POST";
				break;
			default:
				return nullptr;
		}

		MsgPack old_doc;
		if (data.version.empty()) {
			Data current_data;
			try {
				auto current_document = get_document_term(term_id);
				current_data = Data(current_document.get_data());
				data.version = current_document.get_value(DB_SLOT_VERSION);  // update version in data
			} catch (const Xapian::DocNotFoundError&) {
			} catch (const Xapian::DatabaseNotFoundError&) {}
			old_doc = current_data.get_obj();
		} else {
			old_doc = data.get_obj();
		}

		auto doc = std::make_unique<MsgPack>(object);
		L_INDEX("Script: call({}, {})", doc->to_string(4), old_doc.to_string(4));
		(*processor)(http_method, *doc, old_doc, script.get_params());
		return doc;
	}
	return nullptr;
#else
	THROW(ClientError, "Script type 'chai' (ChaiScript) not available.");
#endif
}


std::tuple<std::string, Xapian::Document, MsgPack>
DatabaseHandler::prepare(const MsgPack& document_id, Xapian::rev document_ver, const MsgPack& obj, Data& data)
{
	L_CALL("DatabaseHandler::prepare({}, {}, <data>)", repr(document_id.to_string()), repr(obj.to_string()));

	std::tuple<std::string, Xapian::Document, MsgPack> prepared;

	if (document_ver && !data.version.empty()) {
		if (document_ver != unserialise_length(data.version)) {
			throw Xapian::DocVersionConflictError("Version mismatch!");
		}
	}

	auto schema_begins = std::chrono::system_clock::now();
	do {
		schema = get_schema(&obj);
		L_INDEX("Schema: {}", repr(schema->to_string()));
		prepared = schema->index(obj, document_id, *this, data);
	} while (!update_schema(schema_begins));

	auto& doc = std::get<1>(prepared);
	auto& data_obj = std::get<2>(prepared);

	// Finish document: add data, ID term and ID value.
	// The following flush() **must** be after passing data to Schema::index() as
	// it uses it to get the old document during DatabaseHandler::call_script().
	data.set_obj(data_obj);
	data.flush();
	auto serialised = data.serialise();
	if (!serialised.empty()) {
		doc.set_data(serialised);
	}

	// Request version
	if (document_ver) {
		doc.add_value(DB_SLOT_VERSION, serialise_length(document_ver));
	} else if (!data.version.empty()) {
		doc.add_value(DB_SLOT_VERSION, data.version);
	}

	return prepared;
}


std::tuple<std::string, Xapian::Document, MsgPack>
DatabaseHandler::prepare(const MsgPack& document_id, Xapian::rev document_ver, bool stored, const MsgPack& body, const ct_type_t& ct_type)
{
	L_CALL("DatabaseHandler::prepare({}, {}, {}, {}/{})", repr(document_id.to_string()), stored, repr(body.to_string()), ct_type.first, ct_type.second);

	if ((flags & DB_WRITABLE) != DB_WRITABLE) {
		THROW(Error, "Database is read-only");
	}

	Data data;
	switch (body.getType()) {
		case MsgPack::Type::STR:
			if (stored) {
				data.update(ct_type, -1, 0, 0, body.str_view());
			} else {
				auto blob = body.str_view();
				if (blob.size() > NON_STORED_SIZE_LIMIT) {
					THROW(ClientError, "Non-stored object has a size limit of {}", string::from_bytes(NON_STORED_SIZE_LIMIT));
				}
				data.update(ct_type, blob);
			}
			return prepare(document_id, document_ver, MsgPack::MAP(), data);
		case MsgPack::Type::NIL:
		case MsgPack::Type::UNDEFINED:
			data.erase(ct_type);
			return prepare(document_id, document_ver, MsgPack::MAP(), data);
		case MsgPack::Type::MAP:
			inject_data(data, body);
			return prepare(document_id, document_ver, body, data);
		default:
			THROW(ClientError, "Indexed object must be a JSON, a MsgPack or a blob, is {}", body.getStrType());
	}
}


DataType
DatabaseHandler::index(const MsgPack& document_id, Xapian::rev document_ver, const MsgPack& obj, Data& data, bool commit, bool comments)
{
	L_CALL("DatabaseHandler::index({}, {}, <data>, {})", repr(document_id.to_string()), repr(obj.to_string()), commit, comments);

	auto prepared = prepare(document_id, document_ver, obj, data);
	auto& term_id = std::get<0>(prepared);
	auto& doc = std::get<1>(prepared);
	auto& data_obj = std::get<2>(prepared);

	auto did = replace_document_term(term_id, std::move(doc), commit);

	Document document(did, this);

	auto it_id = data_obj.find(ID_FIELD_NAME);
	if (it_id == data_obj.end()) {
		// TODO: This may be somewhat expensive, but replace_document()
		//       doesn't currently return the "document_id" (not the docid).
		data_obj[ID_FIELD_NAME] = document_id ? document_id : document.get_value(ID_FIELD_NAME);
	} else if (term_id == "QN\x80") {
		// Set id inside serialized object:
		auto& value = it_id.value();
		switch (value.getType()) {
			case MsgPack::Type::POSITIVE_INTEGER:
				value = static_cast<uint64_t>(did);
				break;
			case MsgPack::Type::NEGATIVE_INTEGER:
				value = static_cast<int64_t>(did);
				break;
			case MsgPack::Type::FLOAT:
				value = static_cast<double>(did);
				break;
			default:
				break;
		}
	}

	try {
		// TODO: This may be somewhat expensive, but replace_document()
		//       doesn't currently return the "version".
		auto version = document.get_value(DB_SLOT_VERSION);
		if (!version.empty()) {
			data_obj[RESERVED_VERSION] = static_cast<Xapian::rev>(sortable_unserialise(version));
		}
	} catch(...) {
		L_EXC("Cannot retrieve document version for docid {}!", did);
	}

	if (comments) {
		data_obj[RESPONSE_xDOCID] = did;

		size_t n_shards = endpoints.size();
		size_t shard_num = (did - 1) % n_shards;
		data_obj[RESPONSE_xSHARD] = shard_num;
		// data_obj[RESPONSE_xENDPOINT] = endpoints[shard_num].to_string();
	}

	return std::make_pair(std::move(did), std::move(data_obj));
}


DataType
DatabaseHandler::index(const MsgPack& document_id, Xapian::rev document_ver, bool stored, const MsgPack& body, bool commit, bool comments, const ct_type_t& ct_type)
{
	L_CALL("DatabaseHandler::index({}, {}, {}, {}, {}, {}/{})", repr(document_id.to_string()), stored, repr(body.to_string()), commit, comments, ct_type.first, ct_type.second);

	if ((flags & DB_WRITABLE) != DB_WRITABLE) {
		THROW(Error, "Database is read-only");
	}

	int t = CONFLICT_RETRIES;
	while (true) {
		try {
			Data data;
			switch (body.getType()) {
				case MsgPack::Type::STR:
					if (stored) {
						data.update(ct_type, -1, 0, 0, body.str_view());
					} else {
						auto blob = body.str_view();
						if (blob.size() > NON_STORED_SIZE_LIMIT) {
							THROW(ClientError, "Non-stored object has a size limit of {}", string::from_bytes(NON_STORED_SIZE_LIMIT));
						}
						data.update(ct_type, blob);
					}
					return index(document_id, document_ver, MsgPack::MAP(), data, commit, comments);
				case MsgPack::Type::NIL:
				case MsgPack::Type::UNDEFINED:
					data.erase(ct_type);
					return index(document_id, document_ver, MsgPack::MAP(), data, commit, comments);
				case MsgPack::Type::MAP:
					inject_data(data, body);
					return index(document_id, document_ver, body, data, commit, comments);
				default:
					THROW(ClientError, "Indexed object must be a JSON, a MsgPack or a blob, is {}", body.getStrType());
			}
		} catch (const Xapian::DocVersionConflictError&) {
			if (--t == 0 || document_ver) { throw; }
		}
	}
}


DataType
DatabaseHandler::patch(const MsgPack& document_id, Xapian::rev document_ver, const MsgPack& patches, bool commit, bool comments, const ct_type_t& /*ct_type*/)
{
	L_CALL("DatabaseHandler::patch({}, <patches>, {})", repr(document_id.to_string()), commit);

	if ((flags & DB_WRITABLE) != DB_WRITABLE) {
		THROW(Error, "database is read-only");
	}

	if (!document_id) {
		THROW(ClientError, "Document must have an 'id'");
	}

	if (!patches.is_map() && !patches.is_array()) {
		THROW(ClientError, "Patches must be a JSON or MsgPack");
	}

	const auto term_id = get_prefixed_term_id(document_id);

	int t = CONFLICT_RETRIES;
	while (true) {
		try {
			Data data;
			try {
				auto current_document = get_document_term(term_id);
				data = Data(current_document.get_data(), current_document.get_value(DB_SLOT_VERSION));
			} catch (const Xapian::DocNotFoundError&) {
			} catch (const Xapian::DatabaseNotFoundError&) {}
			auto obj = data.get_obj();

			apply_patch(patches, obj);

			return index(document_id, document_ver, obj, data, commit, comments);
		} catch (const Xapian::DocVersionConflictError&) {
			if (--t == 0 || document_ver) { throw; }
		}
	}
}


DataType
DatabaseHandler::update(const MsgPack& document_id, Xapian::rev document_ver, bool stored, const MsgPack& body, bool commit, bool comments, const ct_type_t& ct_type)
{
	L_CALL("DatabaseHandler::update({}, {}, <body>, {}, {}, {}/{})", repr(document_id.to_string()), stored, commit, comments, ct_type.first, ct_type.second);

	if ((flags & DB_WRITABLE) != DB_WRITABLE) {
		THROW(Error, "database is read-only");
	}

	if (!document_id) {
		THROW(ClientError, "Document must have an 'id'");
	}

	const auto term_id = get_prefixed_term_id(document_id);

	int t = CONFLICT_RETRIES;
	while (true) {
		try {
			Data data;
			try {
				auto current_document = get_document_term(term_id);
				data = Data(current_document.get_data(), current_document.get_value(DB_SLOT_VERSION));
			} catch (const Xapian::DocNotFoundError&) {
			} catch (const Xapian::DatabaseNotFoundError&) {}
			auto obj = data.get_obj();

			switch (body.getType()) {
				case MsgPack::Type::STR:
					if (stored) {
						data.update(ct_type, -1, 0, 0, body.str_view());
					} else {
						auto blob = body.str_view();
						if (blob.size() > NON_STORED_SIZE_LIMIT) {
							THROW(ClientError, "Non-stored object has a size limit of {}", string::from_bytes(NON_STORED_SIZE_LIMIT));
						}
						data.update(ct_type, blob);
					}
					return index(document_id, document_ver, obj, data, commit, comments);
				case MsgPack::Type::NIL:
				case MsgPack::Type::UNDEFINED:
					data.erase(ct_type);
					return index(document_id, document_ver, obj, data, commit, comments);
				case MsgPack::Type::MAP:
					if (stored) {
						THROW(ClientError, "Objects of this type cannot be put in storage");
					}
					if (obj.empty()) {
						inject_data(data, body);
						return index(document_id, document_ver, body, data, commit, comments);
					} else {
						obj.update(body);
						inject_data(data, obj);
						return index(document_id, document_ver, obj, data, commit, comments);
					}
				default:
					THROW(ClientError, "Indexed object must be a JSON, a MsgPack or a blob, is {}", body.getStrType());
			}

			return index(document_id, document_ver, obj, data, commit, comments);
		} catch (const Xapian::DocVersionConflictError&) {
			if (--t == 0 || document_ver) { throw; }
		}
	}
}


void
DatabaseHandler::write_schema(const MsgPack& obj, bool replace)
{
	L_CALL("DatabaseHandler::write_schema({})", repr(obj.to_string()));

	auto schema_begins = std::chrono::system_clock::now();
	bool was_foreign_obj;
	do {
		schema = get_schema();
		was_foreign_obj = schema->write(obj, replace);
		if (!was_foreign_obj && opts.foreign) {
			THROW(ForeignSchemaError, "Schema of {} must use a foreign schema", repr(endpoints.to_string()));
		}
		L_INDEX("Schema to write: {} {}", repr(schema->to_string()), was_foreign_obj ? "(foreign)" : "(local)");
	} while (!update_schema(schema_begins));

	if (was_foreign_obj) {
		MsgPack o = obj;
		o[RESERVED_TYPE] = "object";
		o.erase(RESERVED_ENDPOINT);
		do {
			schema = get_schema();
			was_foreign_obj = schema->write(o, replace);
			L_INDEX("Schema to write: {} (local)", repr(schema->to_string()));
		} while (!update_schema(schema_begins));
	}
}


void
DatabaseHandler::delete_schema()
{
	L_CALL("DatabaseHandler::delete_schema()");

	auto schema_begins = std::chrono::system_clock::now();
	bool done;
	do {
		schema = get_schema();
		auto old_schema = schema->get_const_schema();
		done = XapiandManager::schemas()->drop(this, old_schema);
		L_INDEX("Schema to delete: {}", repr(schema->to_string()));
	} while (!done);
	auto schema_ends = std::chrono::system_clock::now();
	(void)schema_begins;
	(void)schema_ends;
	// Stats::add("schema_updates", std::chrono::duration_cast<std::chrono::nanoseconds>(schema_ends - schema_begins).count());
}


Xapian::RSet
DatabaseHandler::get_rset(const Xapian::Query& query, Xapian::doccount maxitems)
{
	L_CALL("DatabaseHandler::get_rset(...)");

	lock_database lk_db(endpoints, flags);

	// Xapian::RSet only keeps a set of Xapian::docid internally,
	// so it's thread safe across database checkouts.

	Xapian::RSet rset;

	for (int t = DB_RETRIES; t >= 0; --t) {
		try {
			Xapian::Enquire enquire(*lk_db->db());
			enquire.set_query(query);
			auto mset = enquire.get_mset(0, maxitems);
			for (const auto& did : mset) {
				rset.add_document(did);
			}
			break;
		} catch (const Xapian::DatabaseModifiedError& exc) {
			if (t == 0) { throw; }
		} catch (const Xapian::NetworkError& exc) {
			if (t == 0) { throw; }
		} catch (const Xapian::Error& exc) {
			THROW(Error, exc.get_description());
		}
		lk_db->reopen();
	}

	return rset;
}


std::unique_ptr<Xapian::ExpandDecider>
DatabaseHandler::get_edecider(const similar_field_t& similar)
{
	L_CALL("DatabaseHandler::get_edecider(...)");

	// Expand Decider filter.
	std::vector<std::string> prefixes;
	prefixes.reserve(similar.type.size() + similar.field.size());
	for (const auto& sim_type : similar.type) {
		char type = toUType(Unserialise::type(sim_type));
		prefixes.emplace_back(1, type);
		prefixes.emplace_back(1, tolower(type));
	}
	for (const auto& sim_field : similar.field) {
		auto field_spc = schema->get_data_field(sim_field).first;
		if (field_spc.get_type() != FieldType::EMPTY) {
			prefixes.push_back(field_spc.prefix());
		}
	}
	return std::make_unique<FilterPrefixesExpandDecider>(prefixes);
}


void
DatabaseHandler::dump_metadata(int fd)
{
	L_CALL("DatabaseHandler::dump_metadata()");

	lock_database lk_db(endpoints, flags);

	XXH32_state_t* xxh_state = XXH32_createState();
	XXH32_reset(xxh_state, 0);

	auto db_endpoints = endpoints.to_string();
	serialise_string(fd, dump_metadata_header);
	XXH32_update(xxh_state, dump_metadata_header.data(), dump_metadata_header.size());

	serialise_string(fd, db_endpoints);
	XXH32_update(xxh_state, db_endpoints.data(), db_endpoints.size());

	lk_db->dump_metadata(fd, xxh_state);

	uint32_t current_hash = XXH32_digest(xxh_state);
	XXH32_freeState(xxh_state);

	serialise_length(fd, current_hash);
	L_INFO("Dump hash is {:#010x}", current_hash);
}


void
DatabaseHandler::dump_schema(int fd)
{
	L_CALL("DatabaseHandler::dump_schema()");

	schema = get_schema();
	auto saved_schema_ser = schema->get_full().serialise();

	lock_database lk_db(endpoints, flags);

	XXH32_state_t* xxh_state = XXH32_createState();
	XXH32_reset(xxh_state, 0);

	auto db_endpoints = endpoints.to_string();
	serialise_string(fd, dump_schema_header);
	XXH32_update(xxh_state, dump_schema_header.data(), dump_schema_header.size());

	serialise_string(fd, db_endpoints);
	XXH32_update(xxh_state, db_endpoints.data(), db_endpoints.size());

	serialise_string(fd, saved_schema_ser);
	XXH32_update(xxh_state, saved_schema_ser.data(), saved_schema_ser.size());

	uint32_t current_hash = XXH32_digest(xxh_state);
	XXH32_freeState(xxh_state);

	serialise_length(fd, current_hash);
	L_INFO("Dump hash is {:#010x}", current_hash);
}


void
DatabaseHandler::dump_documents(int fd)
{
	L_CALL("DatabaseHandler::dump_documents()");

	lock_database lk_db(endpoints, flags);

	XXH32_state_t* xxh_state = XXH32_createState();
	XXH32_reset(xxh_state, 0);

	auto db_endpoints = endpoints.to_string();
	serialise_string(fd, dump_documents_header);
	XXH32_update(xxh_state, dump_documents_header.data(), dump_documents_header.size());

	serialise_string(fd, db_endpoints);
	XXH32_update(xxh_state, db_endpoints.data(), db_endpoints.size());

	lk_db->dump_documents(fd, xxh_state);

	uint32_t current_hash = XXH32_digest(xxh_state);
	XXH32_freeState(xxh_state);

	serialise_length(fd, current_hash);
	L_INFO("Dump hash is {:#010x}", current_hash);
}


void
DatabaseHandler::restore(int fd)
{
	L_CALL("DatabaseHandler::restore()");

	std::string buffer;
	std::size_t off = 0;
	std::size_t acc = 0;

	lock_database lk_db(endpoints, flags);

	XXH32_state_t* xxh_state = XXH32_createState();
	XXH32_reset(xxh_state, 0);

	auto header = unserialise_string(fd, buffer, off, acc);
	XXH32_update(xxh_state, header.data(), header.size());
	if (header != dump_documents_header && header != dump_schema_header && header != dump_metadata_header) {
		THROW(ClientError, "Invalid dump", RESERVED_TYPE);
	}

	auto db_endpoints = unserialise_string(fd, buffer, off, acc);
	XXH32_update(xxh_state, db_endpoints.data(), db_endpoints.size());

	// restore metadata (key, value)
	if (header == dump_metadata_header) {
		size_t i = 0;
		while (true) {
			++i;
			auto key = unserialise_string(fd, buffer, off, acc);
			XXH32_update(xxh_state, key.data(), key.size());
			auto value = unserialise_string(fd, buffer, off, acc);
			XXH32_update(xxh_state, value.data(), value.size());
			if (key.empty() && value.empty()) {
				break;
			}
			if (key.empty()) {
				L_WARNING("Metadata with no key ignored [{}]", ID_FIELD_NAME, i);
				continue;
			}
			L_INFO_HOOK("DatabaseHandler::restore", "Restoring metadata {} = {}", key, value);
			lk_db->set_metadata(key, value, false, false);
		}
	}

	// restore schema
	if (header == dump_schema_header) {
		auto saved_schema_ser = unserialise_string(fd, buffer, off, acc);
		XXH32_update(xxh_state, saved_schema_ser.data(), saved_schema_ser.size());

		lk_db.unlock();
		if (!saved_schema_ser.empty()) {
			auto saved_schema = MsgPack::unserialise(saved_schema_ser);
			L_INFO_HOOK("DatabaseHandler::restore", "Restoring schema: {}", saved_schema.to_string(4));
			write_schema(saved_schema, true);
		}
		schema = get_schema();
		lk_db.lock();
	}

	// restore documents (document_id, object, blob)
	if (header == dump_documents_header) {
		lk_db.unlock();
		schema = get_schema();

		constexpr size_t limit_max = 16;
		constexpr size_t limit_signal = 8;
		LightweightSemaphore limit(limit_max);
		BlockingConcurrentQueue<std::tuple<std::string, Xapian::Document, MsgPack>> queue;
		std::atomic_bool ready = false;
		std::atomic_size_t accumulated = 0;
		std::atomic_size_t processed = 0;
		std::size_t total = 0;

		ThreadPool<> thread_pool("TP{:02}", 4 * std::thread::hardware_concurrency());

		// Index documents.
		auto indexer = thread_pool.async([&]{
			lock_database lk_db(endpoints, flags, false);
			bool ready_ = false;
			while (true) {
				if (XapiandManager::manager()->is_detaching()) {
					thread_pool.finish();
					break;
				}
				std::tuple<std::string, Xapian::Document, MsgPack> prepared;
				queue.wait_dequeue(prepared);
				if (!ready_) {
					ready_ = ready.load(std::memory_order_relaxed);
				}
				auto processed_ = processed.fetch_add(1, std::memory_order_acquire) + 1;

				auto& term_id = std::get<0>(prepared);
				auto& doc = std::get<1>(prepared);

				if (!term_id.empty()) {
					auto database = lk_db.lock();
					try {
						database->replace_document_term(term_id, std::move(doc), false, false);
					} catch (...) {
						L_EXC("ERROR: Cannot replace document");
					}
					lk_db.unlock();
				}

				if (ready_) {
					if (processed_ >= total) {
						queue.enqueue(std::make_tuple(std::string{}, Xapian::Document{}, MsgPack{}));
						break;
					}
					if (processed_ % (1024 * 16) == 0) {
						L_INFO("{} of {} documents processed ({})", processed_, total, string::from_bytes(accumulated.load(std::memory_order_relaxed)));
					}
				} else {
					if (processed_ % (limit_signal * 32) == 0) {
						limit.signal(limit_signal);
					}
					if (processed_ % (1024 * 16) == 0) {
						L_INFO("{} documents processed ({})", processed_, string::from_bytes(accumulated.load(std::memory_order_relaxed)));
					}
				}
			}
		});

		std::array<std::function<void()>, ConcurrentQueueDefaultTraits::BLOCK_SIZE> bulk;
		size_t bulk_cnt = 0;
		while (true) {
			if (XapiandManager::manager()->is_detaching()) {
				thread_pool.finish();
				break;
			}
			auto obj = MsgPack::MAP();
			Data data;
			try {
				bool eof = true;
				while (true) {
					auto blob = unserialise_string(fd, buffer, off, acc);
					XXH32_update(xxh_state, blob.data(), blob.size());
					if (blob.empty()) { break; }
					auto content_type = unserialise_string(fd, buffer, off, acc);
					XXH32_update(xxh_state, content_type.data(), content_type.size());
					auto type_ser = unserialise_char(fd, buffer, off, acc);
					XXH32_update(xxh_state, &type_ser, 1);
					if (content_type.empty()) {
						obj = MsgPack::unserialise(blob);
					} else {
						switch (static_cast<Locator::Type>(type_ser)) {
							case Locator::Type::inplace:
							case Locator::Type::compressed_inplace:
								data.update(content_type, blob);
								break;
							case Locator::Type::stored:
							case Locator::Type::compressed_stored:
								data.update(content_type, -1, 0, 0, blob);
								break;
						}
					}
					eof = false;
				}
				if (eof) { break; }
				accumulated.store(acc, std::memory_order_relaxed);
			} catch (...) {
				L_EXC("ERROR: Cannot replace document");
				break;
			}

			MsgPack document_id;

			auto f_it = obj.find(ID_FIELD_NAME);
			if (f_it != obj.end()) {
				const auto& field = f_it.value();
				if (field.is_map()) {
					auto f_it_end = field.end();
					auto fv_it = field.find(RESERVED_VALUE);
					if (fv_it != f_it_end) {
						document_id = fv_it.value();
					}
				} else {
					document_id = field;
				}
			}

			if (!document_id) {
				L_WARNING("Skipping document with no valid '{}'", ID_FIELD_NAME);
				continue;
			}

			++total;

			bulk[bulk_cnt++] = [
				&,
				document_id = std::move(document_id),
				obj = std::move(obj),
				data = std::move(data)
			]() mutable {
				try {
					DatabaseHandler db_handler(endpoints, flags, method);
					queue.enqueue(db_handler.prepare(document_id, 0, obj, data));
				} catch (...) {
					L_EXC("ERROR: Cannot prepare document");
					queue.enqueue(std::make_tuple(std::string{}, Xapian::Document{}, MsgPack{}));
				}
			};
			if (bulk_cnt == bulk.size()) {
				if (!thread_pool.enqueue_bulk(bulk.begin(), bulk_cnt)) {
					L_ERR("Ignored {} documents: cannot enqueue tasks!", bulk_cnt);
				}
				bulk_cnt = 0;
				limit.wait();
			}
		}

		if (bulk_cnt != 0) {
			if (!thread_pool.enqueue_bulk(bulk.begin(), bulk_cnt)) {
				L_ERR("Ignored {} documents: cannot enqueue tasks!", bulk_cnt);
			}
		}

		ready.store(true, std::memory_order_release);

		indexer.wait();

		L_INFO("{} of {} documents processed ({})", processed.load(std::memory_order_relaxed), total, string::from_bytes(acc));

		lk_db.lock();
	}

	lk_db->commit(false);

	uint32_t saved_hash = unserialise_length(fd, buffer, off, acc);
	uint32_t current_hash = XXH32_digest(xxh_state);
	XXH32_freeState(xxh_state);

	if (saved_hash != current_hash) {
		L_WARNING("Invalid dump hash. Should be {:#010x}, but instead is {:#010x}", saved_hash, current_hash);
	}
}


MsgPack
DatabaseHandler::dump_documents()
{
	L_CALL("DatabaseHandler::dump_documents()");

	lock_database lk_db(endpoints, flags);

	return lk_db->dump_documents();
}



std::tuple<std::string, Xapian::Document, MsgPack>
DatabaseHandler::prepare_document(MsgPack& body)
{
	L_CALL("DatabaseHandler::prepare_document(<body>)");

	if ((flags & DB_WRITABLE) != DB_WRITABLE) {
		THROW(Error, "Database is read-only");
	}

	if (!body.is_map()) {
		THROW(ClientError, "Object must be a JSON or MsgPack");
	}

	MsgPack document_id;

	auto f_it = body.find(ID_FIELD_NAME);
	if (f_it != body.end()) {
		const auto& field = f_it.value();
		if (field.is_map()) {
			auto f_it_end = field.end();
			auto fv_it = field.find(RESERVED_VALUE);
			if (fv_it != f_it_end) {
				document_id = fv_it.value();
			}
		} else {
			document_id = field;
		}
	}

	std::string op_type = "index";
	f_it = body.find(RESERVED_OP_TYPE);
	if (f_it != body.end()) {
		op_type = f_it.value().as_str();
		body.erase(f_it);
	}

	if (op_type == "index") {
		Data data;
		inject_data(data, body);

		return prepare(document_id, 0, body, data);
	}

	if (op_type == "patch") {
		if (!document_id) {
			THROW(ClientError, "Document must have an 'id'");
		}

		const auto term_id = get_prefixed_term_id(document_id);

		Data data;
		try {
			auto current_document = get_document_term(term_id);
			data = Data(current_document.get_data(), current_document.get_value(DB_SLOT_VERSION));
		} catch (const Xapian::DocNotFoundError&) {
		} catch (const Xapian::DatabaseNotFoundError&) {}
		auto obj = data.get_obj();
		apply_patch(body, obj);

		return prepare(document_id, 0, body, data);
	}

	if (op_type == "update" || op_type == "merge") {
		if (!document_id) {
			THROW(ClientError, "Document must have an 'id'");
		}

		const auto term_id = get_prefixed_term_id(document_id);

		Data data;
		try {
			auto current_document = get_document_term(term_id);
			data = Data(current_document.get_data(), current_document.get_value(DB_SLOT_VERSION));
		} catch (const Xapian::DocNotFoundError&) {
		} catch (const Xapian::DatabaseNotFoundError&) {}
		auto obj = data.get_obj();

		if (obj.empty()) {
			inject_data(data, body);
			return prepare(document_id, 0, body, data);
		} else {
			obj.update(body);
			inject_data(data, obj);
			return prepare(document_id, 0, obj, data);
		}
	}

	THROW(ClientError, "Invalid operation type: {}", repr(op_type));
}


MSet
DatabaseHandler::get_all_mset(const std::string& term, Xapian::docid initial, size_t limit)
{
	L_CALL("DatabaseHandler::get_all_mset()");

	MSet mset{};

	auto term_string = std::string(term);

	lock_database lk_db(endpoints, flags);

	for (int t = DB_RETRIES; t >= 0; --t) {
		try {
			auto it = lk_db->db()->postlist_begin(term_string);
			auto it_e = lk_db->db()->postlist_end(term_string);
			if (initial) {
				it.skip_to(initial);
			}
			for (; it != it_e && limit; ++it, --limit) {
				initial = *it;
				mset.push_back(initial);
			}
			break;
		} catch (const Xapian::DatabaseModifiedError& exc) {
			if (t == 0) { throw; }
		} catch (const Xapian::NetworkError& exc) {
			if (t == 0) { throw; }
		} catch (const QueryParserError& exc) {
			THROW(ClientError, exc.what());
		} catch (const SerialisationError& exc) {
			THROW(ClientError, exc.what());
		} catch (const QueryDslError& exc) {
			THROW(ClientError, exc.what());
		} catch (const Xapian::QueryParserError& exc) {
			THROW(ClientError, exc.get_description());
		} catch (const Xapian::Error& exc) {
			THROW(Error, exc.get_description());
		} catch (const std::exception& exc) {
			THROW(ClientError, "The search was not performed: {}", exc.what());
		}
		lk_db->reopen();
	}

	return mset;
}


MSet
DatabaseHandler::get_mset(const query_field_t& query_field, const MsgPack* qdsl, AggregationMatchSpy* aggs)
{
	L_CALL("DatabaseHandler::get_mset({}, {})", repr(string::join(query_field.query, " & ")), qdsl ? repr(qdsl->to_string()) : "null");

	schema = get_schema();

	auto limit = query_field.limit;
	auto check_at_least = query_field.check_at_least;
	auto offset = query_field.offset;

	QueryDSL query_object(schema);

	Xapian::Query query;
	std::unique_ptr<Multi_MultiValueKeyMaker> sorter;
	switch (method) {
		case HTTP_GET:
		case HTTP_POST: {

			if (qdsl && qdsl->find(RESERVED_QUERYDSL_SORT) != qdsl->end()) {
				auto value = qdsl->at(RESERVED_QUERYDSL_SORT);
				sorter = query_object.get_sorter(value);
			}

			if (qdsl && qdsl->find(RESERVED_QUERYDSL_QUERY) != qdsl->end()) {
				query = query_object.get_query(qdsl->at(RESERVED_QUERYDSL_QUERY));
			} else {
				query = query_object.get_query(query_object.make_dsl_query(query_field));
			}

			if (qdsl && qdsl->find(RESERVED_QUERYDSL_OFFSET) != qdsl->end()) {
				auto value = qdsl->at(RESERVED_QUERYDSL_OFFSET);
				if (value.is_integer()) {
					offset = value.as_u64();
				} else {
					THROW(ClientError, "The {} must be a unsigned int", RESERVED_QUERYDSL_OFFSET);
				}
			}

			if (qdsl && qdsl->find(RESERVED_QUERYDSL_LIMIT) != qdsl->end()) {
				auto value = qdsl->at(RESERVED_QUERYDSL_LIMIT);
				if (value.is_integer()) {
					limit = value.as_u64();
				} else {
					THROW(ClientError, "The {} must be a unsigned int", RESERVED_QUERYDSL_LIMIT);
				}
			}

			if (qdsl && qdsl->find(RESERVED_QUERYDSL_CHECK_AT_LEAST) != qdsl->end()) {
				auto value = qdsl->at(RESERVED_QUERYDSL_CHECK_AT_LEAST);
				if (value.is_integer()) {
					check_at_least = value.as_u64();
				} else {
					THROW(ClientError, "The {} must be a unsigned int", RESERVED_QUERYDSL_CHECK_AT_LEAST);
				}
			}

			break;
		}

		default:
			break;
	}

	// L_DEBUG("query: {}", query.get_description());

	// Configure sorter.
	if (!query_field.sort.empty()) {
		if (!sorter) {
			sorter = std::make_unique<Multi_MultiValueKeyMaker>();
		}
		for (std::string_view sort : query_field.sort) {
			size_t pos = sort.find(':');
			if (pos != std::string_view::npos) {
				auto field = sort.substr(0, pos);
				auto value = sort.substr(pos);
				MsgPack sort_obj;
				if (!query_field.metric.empty()) {
					if (field[0] == '-') {
						field = field.substr(1, field.size());
						sort_obj = MsgPack({{ field, {{ RESERVED_VALUE, value }, { RESERVED_QUERYDSL_METRIC , query_field.metric}, { RESERVED_QUERYDSL_ORDER , QUERYDSL_DESC }} }});
					} else {
						sort_obj = MsgPack({{ field, {{ RESERVED_VALUE, value }, { RESERVED_QUERYDSL_METRIC , query_field.metric}} }});
					}
				} else {
					if (field[0] == '-') {
						field = field.substr(1, field.size());
						sort_obj = MsgPack({{ field, {{ RESERVED_VALUE, value }, { RESERVED_QUERYDSL_ORDER , QUERYDSL_DESC }} }});
					} else {
						sort_obj = MsgPack({{ field, {{ RESERVED_VALUE, value }} }});
					}
				}

				query_object.get_sorter(sorter, sort_obj);
			} else {
				query_object.get_sorter(sorter, sort);
			}
		}
	}

	// Get the collapse key to use for queries.
	Xapian::valueno collapse_key = Xapian::BAD_VALUENO;
	if (!query_field.collapse.empty()) {
		const auto field_spc = schema->get_slot_field(query_field.collapse);
		collapse_key = field_spc.slot;
	}

	// Configure nearest and fuzzy search:
	std::unique_ptr<Xapian::ExpandDecider> nearest_edecider;
	Xapian::RSet nearest_rset;
	if (query_field.is_nearest) {
		nearest_edecider = get_edecider(query_field.nearest);
		nearest_rset = get_rset(query, query_field.nearest.n_rset);
	}

	Xapian::RSet fuzzy_rset;
	std::unique_ptr<Xapian::ExpandDecider> fuzzy_edecider;
	if (query_field.is_fuzzy) {
		fuzzy_edecider = get_edecider(query_field.fuzzy);
		fuzzy_rset = get_rset(query, query_field.fuzzy.n_rset);
	}

	MSet mset;

	lock_database lk_db(endpoints, flags);
	for (int t = DB_RETRIES; t >= 0; --t) {
		try {
			auto final_query = query;
			Xapian::Enquire enquire(*lk_db->db());
			if (collapse_key != Xapian::BAD_VALUENO) {
				enquire.set_collapse_key(collapse_key, query_field.collapse_max);
			}
			if (aggs) {
				enquire.add_matchspy(aggs);
			}
			if (sorter) {
				enquire.set_sort_by_key_then_relevance(sorter.get(), false);
			}
			if (query_field.is_nearest) {
				auto eset = enquire.get_eset(query_field.nearest.n_eset, nearest_rset, nearest_edecider.get());
				final_query = Xapian::Query(Xapian::Query::OP_ELITE_SET, eset.begin(), eset.end(), query_field.nearest.n_term);
			}
			if (query_field.is_fuzzy) {
				auto eset = enquire.get_eset(query_field.fuzzy.n_eset, fuzzy_rset, fuzzy_edecider.get());
				final_query = Xapian::Query(Xapian::Query::OP_OR, final_query, Xapian::Query(Xapian::Query::OP_ELITE_SET, eset.begin(), eset.end(), query_field.fuzzy.n_term));
			}
			enquire.set_query(final_query);
			mset = enquire.get_mset(offset, limit, check_at_least);
			break;
		} catch (const Xapian::DatabaseModifiedError& exc) {
			if (t == 0) { throw; }
		} catch (const Xapian::NetworkError& exc) {
			if (t == 0) { throw; }
		} catch (const QueryParserError& exc) {
			THROW(ClientError, exc.what());
		} catch (const SerialisationError& exc) {
			THROW(ClientError, exc.what());
		} catch (const QueryDslError& exc) {
			THROW(ClientError, exc.what());
		} catch (const Xapian::QueryParserError& exc) {
			THROW(ClientError, exc.get_description());
		} catch (const Xapian::Error& exc) {
			THROW(Error, exc.get_description());
		} catch (const std::exception& exc) {
			THROW(ClientError, "The search was not performed: {}", exc.what());
		}
		lk_db->reopen();
	}

	return mset;
}


MSet
DatabaseHandler::get_mset(const Xapian::Query& query, unsigned offset, unsigned limit, unsigned check_at_least, Xapian::KeyMaker* sorter, Xapian::MatchSpy* spy)
{
	L_CALL("DatabaseHandler::get_mset({}, {}, {}, {})", query.get_description(), offset, limit, check_at_least);

	MSet mset;

	lock_database lk_db(endpoints, flags);
	for (int t = DB_RETRIES; t >= 0; --t) {
		try {
			auto final_query = query;
			Xapian::Enquire enquire(*lk_db->db());
			if (spy) {
				enquire.add_matchspy(spy);
			}
			if (sorter) {
				enquire.set_sort_by_key_then_relevance(sorter, false);
			}
			enquire.set_query(final_query);
			mset = enquire.get_mset(offset, limit, check_at_least);
			break;
		} catch (const Xapian::DatabaseModifiedError& exc) {
			if (t == 0) { throw; }
		} catch (const Xapian::NetworkError& exc) {
			if (t == 0) { throw; }
		} catch (const QueryParserError& exc) {
			THROW(ClientError, exc.what());
		} catch (const SerialisationError& exc) {
			THROW(ClientError, exc.what());
		} catch (const QueryDslError& exc) {
			THROW(ClientError, exc.what());
		} catch (const Xapian::QueryParserError& exc) {
			THROW(ClientError, exc.get_description());
		} catch (const Xapian::Error& exc) {
			THROW(Error, exc.get_description());
		} catch (const std::exception& exc) {
			THROW(ClientError, "The search was not performed: {}", exc.what());
		}
		lk_db->reopen();
	}

	return mset;
}


bool
DatabaseHandler::update_schema(std::chrono::time_point<std::chrono::system_clock> schema_begins)
{
	L_CALL("DatabaseHandler::update_schema()");
	bool done = true;
	bool updated = false;
	bool created = false;

	auto mod_schema = schema->get_modified_schema();
	if (mod_schema) {
		updated = true;
		auto old_schema = schema->get_const_schema();
		done = XapiandManager::schemas()->set(this, old_schema, mod_schema);
		if (done) {
			created = old_schema->at("schema").empty();
		}
	}

	if (done) {
		auto schema_ends = std::chrono::system_clock::now();
		(void)schema_begins;
		(void)schema_ends;
		if (updated) {
			L_DEBUG("Schema for {} {}", repr(endpoints.to_string()), created ? "created" : "updated");
			// Stats::add("schema_updates", std::chrono::duration_cast<std::chrono::nanoseconds>(schema_ends - schema_begins).count());
		} else {
			// Stats::add("schema_reads", std::chrono::duration_cast<std::chrono::nanoseconds>(schema_ends - schema_begins).count());
		}
	}

	return done;
}


std::string
DatabaseHandler::get_prefixed_term_id(const MsgPack& document_id)
{
	L_CALL("DatabaseHandler::get_prefixed_term_id({})", repr(document_id.to_string()));

	schema = get_schema();

	std::string unprefixed_term_id;
	auto spc_id = schema->get_data_id();
	auto id_type = spc_id.get_type();
	if (id_type == FieldType::EMPTY) {
		// Search like namespace.
		const auto type_ser = Serialise::guess_serialise(document_id);
		id_type = type_ser.first;
		if (id_type == FieldType::TEXT || id_type == FieldType::STRING) {
			id_type = FieldType::KEYWORD;
		}
		spc_id.set_type(id_type);
		unprefixed_term_id = type_ser.second;
	} else {
		unprefixed_term_id = Serialise::serialise(spc_id, Cast::cast(id_type, document_id));
	}
	return prefixed(unprefixed_term_id, spc_id.prefix(), spc_id.get_ctype());
}


std::vector<std::string>
DatabaseHandler::get_metadata_keys()
{
	L_CALL("DatabaseHandler::get_metadata_keys()");

	lock_database lk_db(endpoints, flags);
	return lk_db->get_metadata_keys();
}


std::string
DatabaseHandler::get_metadata(const std::string& key)
{
	L_CALL("DatabaseHandler::get_metadata({})", repr(key));

	lock_database lk_db(endpoints, flags);
	return lk_db->get_metadata(key);
}


std::string
DatabaseHandler::get_metadata(std::string_view key)
{
	return get_metadata(std::string(key));
}


bool
DatabaseHandler::set_metadata(const std::string& key, const std::string& value, bool commit, bool overwrite)
{
	L_CALL("DatabaseHandler::set_metadata({}, {}, {}, {})", repr(key), repr(value), commit, overwrite);

	lock_database lk_db(endpoints, flags);
	if (!overwrite) {
		auto old_value = lk_db->get_metadata(key);
		if (!old_value.empty()) {
			return (old_value == value);
		}
	}
	lk_db->set_metadata(key, value, commit);
	return true;
}


bool
DatabaseHandler::set_metadata(std::string_view key, std::string_view value, bool commit, bool overwrite)
{
	return set_metadata(std::string(key), std::string(value), commit, overwrite);
}


Document
DatabaseHandler::get_document(Xapian::docid did)
{
	L_CALL("DatabaseHandler::get_document((Xapian::docid){})", did);

	return Document(did, this);
}


Document
DatabaseHandler::get_document(std::string_view document_id)
{
	L_CALL("DatabaseHandler::get_document((std::string){})", repr(document_id));

	auto did = to_docid(document_id);
	if (did != 0u) {
		return get_document(did);
	}

	const auto term_id = get_prefixed_term_id(document_id);

	lock_database lk_db(endpoints, flags);
	did = lk_db->find_document(term_id);
	return Document(did, this);
}


Xapian::docid
DatabaseHandler::get_docid(std::string_view document_id)
{
	L_CALL("DatabaseHandler::get_docid({})", repr(document_id));

	auto did = to_docid(document_id);
	if (did != 0u) {
		return did;
	}

	const auto term_id = get_prefixed_term_id(document_id);

	lock_database lk_db(endpoints, flags);
	return lk_db->find_document(term_id);
}


void
DatabaseHandler::delete_document(std::string_view document_id, bool commit)
{
	L_CALL("DatabaseHandler::delete_document({})", repr(document_id));

	auto did = to_docid(document_id);
	if (did != 0u) {
		lock_database lk_db(endpoints, flags);
		lk_db->delete_document(did, commit);
		return;
	}

	const auto term_id = get_prefixed_term_id(document_id);

	delete_document_term(term_id, commit);
}


void
DatabaseHandler::delete_document_term(const std::string& term, bool commit)
{
	L_CALL("DatabaseHandler::delete_document_term({})", repr(term));

	lock_database lk_db(endpoints, flags);
	return lk_db->delete_document_term(term, commit);
}


Xapian::docid
DatabaseHandler::replace_document(Xapian::docid did, Xapian::Document&& doc, bool commit)
{
	L_CALL("DatabaseHandler::replace_document({}, <doc>)", did);

	lock_database lk_db(endpoints, flags);
	return lk_db->replace_document(did, std::move(doc), commit);
}


Xapian::docid
DatabaseHandler::replace_document(std::string_view document_id, Xapian::Document&& doc, bool commit)
{
	L_CALL("DatabaseHandler::replace_document({}, <doc>)", repr(document_id));

	auto did = to_docid(document_id);
	if (did != 0u) {
		lock_database lk_db(endpoints, flags);
		return lk_db->replace_document(did, std::move(doc), commit);
	}

	const auto term_id = get_prefixed_term_id(document_id);

	return replace_document_term(term_id, std::move(doc), commit);
}


Xapian::docid
DatabaseHandler::replace_document_term(const std::string& term, Xapian::Document&& doc, bool commit)
{
	L_CALL("DatabaseHandler::replace_document_term({}, <doc>)", repr(term));

	lock_database lk_db(endpoints, flags);
	return lk_db->replace_document_term(term, std::move(doc), commit);
}


MsgPack
DatabaseHandler::get_document_info(std::string_view document_id, bool raw_data)
{
	L_CALL("DatabaseHandler::get_document_info({}, {})", repr(document_id), raw_data);

	auto document = get_document(document_id);
	const auto data = Data(document.get_data());

	MsgPack info;

	info[RESPONSE_DOCID] = document.get_docid();

	if (raw_data) {
		info[RESPONSE_RAW_DATA] = data.serialise();
	}

	auto& info_data = info[RESPONSE_DATA];
	if (!data.empty()) {
		for (auto& locator : data) {
			switch (locator.type) {
				case Locator::Type::inplace:
				case Locator::Type::compressed_inplace:
					if (locator.ct_type.empty()) {
						info_data.push_back(MsgPack({
							{ RESPONSE_CONTENT_TYPE, MSGPACK_CONTENT_TYPE },
							{ RESPONSE_TYPE, "inplace" },
						}));
					} else {
						info_data.push_back(MsgPack({
							{ RESPONSE_CONTENT_TYPE, locator.ct_type.to_string() },
							{ RESPONSE_TYPE, "inplace" },
						}));
					}
					break;
				case Locator::Type::stored:
				case Locator::Type::compressed_stored:
					info_data.push_back(MsgPack({
						{ RESPONSE_CONTENT_TYPE, locator.ct_type.to_string() },
						{ RESPONSE_TYPE, "stored" },
						{ RESPONSE_VOLUME, locator.volume },
						{ RESPONSE_OFFSET, locator.offset },
						{ RESPONSE_SIZE, locator.size },
					}));
					break;
			}
		}
	}

	info[RESPONSE_TERMS] = document.get_terms();
	info[RESPONSE_VALUES] = document.get_values();

	return info;
}


MsgPack
DatabaseHandler::get_database_info()
{
	L_CALL("DatabaseHandler::get_database_info()");

	lock_database lk_db(endpoints, flags);
	auto db = lk_db->db();

	auto doccount = db->get_doccount();
	auto lastdocid = db->get_lastdocid();
	MsgPack info;
	info[RESPONSE_UUID] = db->get_uuid();
	info[RESPONSE_REVISION] = db->get_revision();
	info[RESPONSE_DOC_COUNT] = doccount;
	info[RESPONSE_LAST_ID] = lastdocid;
	info[RESPONSE_DOC_DEL] = lastdocid - doccount;
	info[RESPONSE_AV_LENGTH] = db->get_avlength();
	info[RESPONSE_DOC_LEN_LOWER] =  db->get_doclength_lower_bound();
	info[RESPONSE_DOC_LEN_UPPER] = db->get_doclength_upper_bound();
	info[RESPONSE_HAS_POSITIONS] = db->has_positions();
	return info;
}


#ifdef XAPIAND_DATA_STORAGE
std::string
DatabaseHandler::storage_get_stored(const Locator& locator, Xapian::docid did)
{
	L_CALL("DatabaseHandler::storage_get_stored()");

	lock_database lk_db(endpoints, flags);
	return lk_db->storage_get_stored(locator, did);
}
#endif /* XAPIAND_DATA_STORAGE */


bool
DatabaseHandler::commit(bool wal)
{
	L_CALL("DatabaseHandler::commit({})", wal);

	lock_database lk_db(endpoints, flags);
	return lk_db->commit(wal);
}


bool
DatabaseHandler::reopen()
{
	L_CALL("DatabaseHandler::reopen()");

	lock_database lk_db(endpoints, flags);
	return lk_db->reopen();
}


//  ____             ___           _
// |  _ \  ___   ___|_ _|_ __   __| | _____  _____ _ __
// | | | |/ _ \ / __|| || '_ \ / _` |/ _ \ \/ / _ \ '__|
// | |_| | (_) | (__ | || | | | (_| |  __/>  <  __/ |
// |____/ \___/ \___|___|_| |_|\__,_|\___/_/\_\___|_|

void
DocPreparer::operator()()
{
	L_CALL("DocPreparer::operator()()");

	ASSERT(indexer);
	if (indexer->running) {
		auto http_errors = catch_http_errors([&]{
			DatabaseHandler db_handler(indexer->endpoints, indexer->flags, indexer->method);
			auto prepared = db_handler.prepare_document(obj);
			indexer->ready_queue.enqueue(std::make_tuple(std::move(std::get<0>(prepared)), std::move(std::get<1>(prepared)), std::move(std::get<2>(prepared)), idx));
			return 0;
		});
		if (http_errors.error_code != HTTP_STATUS_OK) {
			indexer->ready_queue.enqueue(std::make_tuple(std::string{}, Xapian::Document{}, indexer->comments ? MsgPack{
				{ RESPONSE_xSTATUS, static_cast<unsigned>(http_errors.error_code) },
				{ RESPONSE_xMESSAGE, string::split(http_errors.error, '\n') }
			} : MsgPack::MAP(), idx));
		}
	}
}


void
DocIndexer::operator()()
{
	L_CALL("DocIndexer::operator()()");

	DatabaseHandler db_handler(endpoints, flags, method);
	bool ready_ = false;
	while (running) {
		std::tuple<std::string, Xapian::Document, MsgPack, size_t> prepared;
		auto valid = ready_queue.wait_dequeue_timed(prepared, 100000);  // wait 100ms

		if (!ready_) {
			ready_ = ready.load(std::memory_order_relaxed);
		}

		size_t processed_;
		if (valid) {
			processed_ = _processed.fetch_add(1, std::memory_order_acquire) + 1;

			auto& term_id = std::get<0>(prepared);
			auto& doc = std::get<1>(prepared);
			auto& data_obj = std::get<2>(prepared);
			auto& idx = std::get<3>(prepared);

			MsgPack obj;
			if (!term_id.empty()) {
				auto http_errors = catch_http_errors([&]{
					auto did = db_handler.replace_document_term(term_id, std::move(doc), false);

					Document document(did, &db_handler);

					auto it_id = data_obj.find(ID_FIELD_NAME);
					if (it_id == data_obj.end()) {
						// TODO: This may be somewhat expensive, but replace_document()
						//       doesn't currently return the "document_id" (not the docid).
						obj[ID_FIELD_NAME] = document.get_value(ID_FIELD_NAME);
					} else if (term_id == "QN\x80") {
						// Set id inside serialized object:
						auto& value = it_id.value();
						switch (value.getType()) {
							case MsgPack::Type::POSITIVE_INTEGER:
								value = static_cast<uint64_t>(did);
								break;
							case MsgPack::Type::NEGATIVE_INTEGER:
								value = static_cast<int64_t>(did);
								break;
							case MsgPack::Type::FLOAT:
								value = static_cast<double>(did);
								break;
							default:
								break;
						}
						obj[ID_FIELD_NAME] = value;
					} else {
						auto& value = it_id.value();
						obj[ID_FIELD_NAME] = value;
					}

					try {
						// TODO: This may be somewhat expensive, but replace_document()
						//       doesn't currently return the "version".
						auto version = document.get_value(DB_SLOT_VERSION);
						if (!version.empty()) {
							obj[RESERVED_VERSION] = static_cast<Xapian::rev>(sortable_unserialise(version));
						}
					} catch(...) {
						L_EXC("Cannot retrieve document version for docid {}!", did);
					}

					if (comments) {
						obj[RESPONSE_xDOCID] = did;

						size_t n_shards = endpoints.size();
						size_t shard_num = (did - 1) % n_shards;
						obj[RESPONSE_xSHARD] = shard_num;
						// obj[RESPONSE_xENDPOINT] = endpoints[shard_num].to_string();
					}

					++_indexed;
					return 0;
				});
				if (http_errors.error_code != HTTP_STATUS_OK) {
					if (comments) {
						obj[RESPONSE_xSTATUS] = static_cast<unsigned>(http_errors.error_code);
						obj[RESPONSE_xMESSAGE] = string::split(http_errors.error, '\n');
					}
				}
			} else if (!data_obj.is_undefined()) {
				obj = std::move(data_obj);
			}
			std::lock_guard<std::mutex> lk(_results_mtx);
			if (_idx > _results.size()) {
				_results.resize(_idx, MsgPack::MAP());
			}
			_results[idx] = std::move(obj);
		} else {
			processed_ = _processed.load(std::memory_order_acquire);
		}

		if (ready_) {
			if (processed_ >= _total) {
				done.signal();
				break;
			}
		} else {
			if (processed_ % (limit_signal * 32) == 0) {
				limit.signal(limit_signal);
			}
		}
	}
}


void
DocIndexer::_prepare(MsgPack&& obj)
{
	L_CALL("DocIndexer::_prepare(<obj>)");

	if (!obj.is_map()) {
		L_ERR("Indexing object has an unsupported type: {}", obj.getStrType());
		return;
	}

	bulk[bulk_cnt++] = DocPreparer::make_unique(shared_from_this(), std::move(obj), _idx++);
	if (bulk_cnt == bulk.size()) {
		_total += bulk_cnt;
		if (!XapiandManager::doc_preparer_pool()->enqueue_bulk(bulk.begin(), bulk_cnt)) {
			_total -= bulk_cnt;
			L_ERR("Ignored {} documents: cannot enqueue tasks!", bulk_cnt);
		}
		if (_total == bulk_cnt) {
			XapiandManager::doc_indexer_pool()->enqueue(shared_from_this());
		}
		bulk_cnt = 0;
		limit.wait();  // throttle the prepare
	}
}


void
DocIndexer::prepare(MsgPack&& obj)
{
	L_CALL("DocIndexer::prepare(<obj>)");

	if (obj.is_array()) {
		for (auto &o : obj) {
			_prepare(std::move(o));
		}
	} else {
		_prepare(std::move(obj));
	}
}


bool
DocIndexer::wait(double timeout)
{
	L_CALL("DocIndexer::wait(<timeout>)");

	if (bulk_cnt != 0) {
		_total += bulk_cnt;
		if (!XapiandManager::doc_preparer_pool()->enqueue_bulk(bulk.begin(), bulk_cnt)) {
			_total -= bulk_cnt;
			L_ERR("Ignored {} documents: cannot enqueue tasks!", bulk_cnt);
		}
		if (_total == bulk_cnt) {
			XapiandManager::doc_indexer_pool()->enqueue(shared_from_this());
		}
		bulk_cnt = 0;
	}

	ready.store(true, std::memory_order_release);

	{
		std::lock_guard<std::mutex> lk(_results_mtx);
		if (_idx > _results.size()) {
			_results.resize(_idx, MsgPack::MAP());
		}
	}

	if (_total && timeout) {
		if (timeout > 0.0) {
			return done.wait(timeout * 1e6);
		} else {
			done.wait();
		}
	}

	return _processed >= _total;
}


void
DocIndexer::finish()
{
	L_CALL("DocIndexer::finish()");

	running = false;
	ready_queue.enqueue(std::make_tuple(std::string{}, Xapian::Document{}, MsgPack{}, 0));
}


//  ____                                        _
// |  _ \  ___   ___ _   _ _ __ ___   ___ _ __ | |_
// | | | |/ _ \ / __| | | | '_ ` _ \ / _ \ '_ \| __|
// | |_| | (_) | (__| |_| | | | | | |  __/ | | | |_
// |____/ \___/ \___|\__,_|_| |_| |_|\___|_| |_|\__|
//

Document::Document()
	: did(0),
	  db_handler(nullptr) { }


Document::Document(const Xapian::Document& doc_)
	: did(doc_.get_docid()),
	  db_handler(nullptr) { }


Document::Document(Xapian::docid did_, DatabaseHandler* db_handler_)
	: did(did_),
	  db_handler(db_handler_) { }


Xapian::docid
Document::get_docid()
{
	return did;
}


std::string
Document::serialise(size_t retries)
{
	L_CALL("Document::serialise({})", retries);

	try {
		if (db_handler != nullptr) {
			lock_database lk_db(db_handler->endpoints, db_handler->flags);
			auto doc = lk_db->get_document(did, true);
			return doc.serialise();
		}
		return Xapian::Document{}.serialise();
	} catch (const Xapian::DatabaseModifiedError& exc) {
		if (retries == 0) { throw; }
		return serialise(--retries);
	}
}


std::string
Document::get_value(Xapian::valueno slot, size_t retries)
{
	L_CALL("Document::get_value({}, {})", slot, retries);

	try {
		if (db_handler != nullptr) {
			lock_database lk_db(db_handler->endpoints, db_handler->flags);
			auto doc = lk_db->get_document(did, true);
			return doc.get_value(slot);
		}
		return "";
	} catch (const Xapian::DatabaseModifiedError& exc) {
		if (retries == 0) { throw; }
		return get_value(slot, --retries);
	}
}


std::string
Document::get_data(size_t retries)
{
	L_CALL("Document::get_data({})", retries);

	try {
		if (db_handler != nullptr) {
			lock_database lk_db(db_handler->endpoints, db_handler->flags);
			auto doc = lk_db->get_document(did, true);
			return doc.get_data();
		}
		return "";
	} catch (const Xapian::DatabaseModifiedError& exc) {
		if (retries == 0) { throw; }
		return get_data(--retries);
	}
}


MsgPack
Document::get_terms(size_t retries)
{
	L_CALL("get_terms({})", retries);

	try {
		MsgPack terms;
		if (db_handler != nullptr) {
			lock_database lk_db(db_handler->endpoints, db_handler->flags);
			auto doc = lk_db->get_document(did, true);
			const auto it_e = doc.termlist_end();
			for (auto it = doc.termlist_begin(); it != it_e; ++it) {
				auto& term = terms[*it];
				term[RESPONSE_WDF] = it.get_wdf();  // The within-document-frequency of the current term in the current document.
				try {
					auto _term_freq = it.get_termfreq();  // The number of documents which this term indexes.
					term[RESPONSE_TERM_FREQ] = _term_freq;
				} catch (const Xapian::InvalidOperationError&) { }  // Iterator has moved, and does not support random access or doc is not associated with a database.
				if (it.positionlist_count() != 0u) {
					auto& term_pos = term[RESPONSE_POS];
					term_pos.reserve(it.positionlist_count());
					const auto pit_e = it.positionlist_end();
					for (auto pit = it.positionlist_begin(); pit != pit_e; ++pit) {
						term_pos.push_back(*pit);
					}
				}
			}
		}
		return terms;
	} catch (const Xapian::DatabaseModifiedError& exc) {
		if (retries == 0) { throw; }
		return get_terms(--retries);
	}
}


MsgPack
Document::get_values(size_t retries)
{
	L_CALL("get_values({})", retries);

	try {
		MsgPack values;
		if (db_handler != nullptr) {
			lock_database lk_db(db_handler->endpoints, db_handler->flags);
			auto doc = lk_db->get_document(did, true);
			values.reserve(doc.values_count());
			const auto iv_e = doc.values_end();
			for (auto iv = doc.values_begin(); iv != iv_e; ++iv) {
				values[std::to_string(iv.get_valueno())] = *iv;
			}
		}
		return values;
	} catch (const Xapian::DatabaseModifiedError& exc) {
		if (retries == 0) { throw; }
		return get_values(--retries);
	}
}


MsgPack
Document::get_value(std::string_view slot_name)
{
	L_CALL("Document::get_value({})", repr(slot_name));

	if (db_handler != nullptr) {
		auto slot_field = db_handler->get_schema()->get_slot_field(slot_name);
		return Unserialise::MsgPack(slot_field.get_type(), get_value(slot_field.slot));
	}
	return MsgPack::NIL();
}


MsgPack
Document::get_obj()
{
	L_CALL("Document::get_obj()");

	auto data = Data(get_data());
	return data.get_obj();
}


MsgPack
Document::get_field(std::string_view slot_name)
{
	L_CALL("Document::get_field({})", repr(slot_name));

	return get_field(slot_name, get_obj());
}


MsgPack
Document::get_field(std::string_view slot_name, const MsgPack& obj)
{
	L_CALL("Document::get_field({}, <obj>)", repr(slot_name));

	auto itf = obj.find(slot_name);
	if (itf != obj.end()) {
		const auto& value = itf.value();
		if (value.is_map()) {
			auto itv = value.find(RESERVED_VALUE);
			if (itv != value.end()) {
				return itv.value();
			}
		}
		return value;
	}

	return MsgPack::NIL();
}


uint64_t
Document::hash(size_t retries)
{
	try {
		uint64_t hash = 0;
		if (db_handler != nullptr) {
			lock_database lk_db(db_handler->endpoints, db_handler->flags);
			auto doc = lk_db->get_document(did, true);

			// Add hash of values
			const auto iv_e = doc.values_end();
			for (auto iv = doc.values_begin(); iv != iv_e; ++iv) {
				hash ^= xxh64::hash(*iv) * iv.get_valueno();
			}

			// Add hash of terms
			const auto it_e = doc.termlist_end();
			for (auto it = doc.termlist_begin(); it != it_e; ++it) {
				hash ^= xxh64::hash(*it) * it.get_wdf();
				const auto pit_e = it.positionlist_end();
				for (auto pit = it.positionlist_begin(); pit != pit_e; ++pit) {
					hash ^= *pit;
				}
			}

			// Add hash of data
			hash ^= xxh64::hash(doc.get_data());
		}
		return hash;
	} catch (const Xapian::DatabaseModifiedError& exc) {
		if (retries == 0) { throw; }
		return hash(--retries);
	}
}


void
committer_commit(std::weak_ptr<Shard> weak_database) {
	if (auto shard = weak_database.lock()) {
		auto start = std::chrono::system_clock::now();

		std::string error;

		try {
			lock_shard lk_shard(Endpoint{shard->endpoint}, DB_WRITABLE);
			lk_shard->commit();
		} catch (const Exception& exc) {
			error = exc.get_message();
		} catch (const Xapian::Error& exc) {
			error = exc.get_description();
		}

		auto end = std::chrono::system_clock::now();

		if (error.empty()) {
			L_DEBUG("Autocommit of {} succeeded after {}", repr(shard->to_string()), string::from_delta(start, end));
		} else {
			L_WARNING("Autocommit of {} falied after {}: {}", repr(shard->to_string()), string::from_delta(start, end), error);
		}
	}
}