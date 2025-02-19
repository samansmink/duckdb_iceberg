
#pragma once

#include "duckdb/common/types.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"

namespace duckdb {
struct IRCCredentials;

struct IRCAPIColumnDefinition {
	string name;
	string type_text;
	idx_t precision;
	idx_t scale;
	idx_t position;
};

struct IRCAPITable {
	string table_id;

	string name;
	string catalog_name;
	string schema_name;
	string table_type;
	string data_source_format;
	string storage_location;

	vector<IRCAPIColumnDefinition> columns;
};

struct IRCAPISchema {
	string schema_name;
	string catalog_name;
};

struct IRCAPITableCredentials {
	string key_id;
	string secret;
	string session_token;
};

class IRCAPI {
public:
	static const string API_VERSION_1;

  	//! WARNING: not thread-safe. To be called once on extension initialization
  	static void InitializeCurl();

	// The {prefix} for a catalog is always optional according to the iceberg spec. So no need to
	// add it if it is not defined.
	static string GetOptionallyPrefixedURL(const string &api_version, const string &prefix);
	static IRCAPITableCredentials GetTableCredentials(ClientContext &context,const string &internal, const string &schema, const string &table, IRCCredentials credentials);
	static vector<string> GetCatalogs(ClientContext &context, const string &catalog, IRCCredentials credentials);
	static vector<IRCAPITable> GetTables(ClientContext &context, const string &catalog, const string &internal, const string &schema, IRCCredentials credentials);
	static IRCAPITable GetTable(ClientContext &context, const string &catalog, const string &internal, const string &schema, const string &table_name, optional_ptr<IRCCredentials> credentials);
	static vector<IRCAPISchema> GetSchemas(ClientContext &context, const string &catalog, const string &internal, IRCCredentials credentials);
	static vector<IRCAPITable> GetTablesInSchema(ClientContext &context, const string &catalog, const string &schema, IRCCredentials credentials);
	static string GetToken(ClientContext &context, string id, string secret, string endpoint);
	static IRCAPISchema CreateSchema(ClientContext &context, const string &catalog, const string &internal, const string &schema, IRCCredentials credentials);
	static void DropSchema(ClientContext &context, const string &internal, const string &schema, IRCCredentials credentials);
	static IRCAPITable CreateTable(ClientContext &context, const string &catalog, const string &internal, const string &schema, IRCCredentials credentials, CreateTableInfo *table_info);
	static void DropTable(ClientContext &context, const string &catalog, const string &internal, const string &schema, string &table_name, IRCCredentials credentials);
};

} // namespace duckdb
