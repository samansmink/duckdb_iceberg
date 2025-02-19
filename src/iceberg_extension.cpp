#define DUCKDB_EXTENSION_MAIN

#include "iceberg_extension.hpp"
#include "storage/irc_catalog.hpp"
#include "storage/irc_transaction_manager.hpp"

#include "duckdb.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/extension_util.hpp"
#include "duckdb/catalog/catalog_entry/macro_catalog_entry.hpp"
#include "duckdb/catalog/default/default_functions.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "iceberg_functions.hpp"
#include "yyjson.hpp"
#include "duckdb/main/extension_util.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>
#include "catalog_api.hpp"
#include <aws/core/Aws.h>
#include <aws/s3/S3Client.h>

namespace duckdb {

static unique_ptr<BaseSecret> CreateCatalogSecretFunction(ClientContext &context, CreateSecretInput &input) {
	// apply any overridden settings
	vector<string> prefix_paths;
	auto result = make_uniq<KeyValueSecret>(prefix_paths, "iceberg", "config", input.name);
	
	for (const auto &named_param : input.options) {
		auto lower_name = StringUtil::Lower(named_param.first);

		if (lower_name == "client_id" || 
				lower_name == "client_secret" ||
				lower_name == "endpoint" ||
				lower_name == "aws_region") {
			result->secret_map[lower_name] = named_param.second.ToString();
		} else {
			throw InternalException("Unknown named parameter passed to CreateIRCSecretFunction: " + lower_name);
		}
	}

	// Get token from catalog
	result->secret_map["token"] = IRCAPI::GetToken(
		context,
		result->secret_map["client_id"].ToString(), 
		result->secret_map["client_secret"].ToString(),
		result->secret_map["endpoint"].ToString());
	
	//! Set redact keys
	result->redact_keys = {"token", "client_id", "client_secret"};

	return std::move(result);
}

static void SetCatalogSecretParameters(CreateSecretFunction &function) {
	function.named_parameters["client_id"] = LogicalType::VARCHAR;
	function.named_parameters["client_secret"] = LogicalType::VARCHAR;
	function.named_parameters["endpoint"] = LogicalType::VARCHAR;
	function.named_parameters["aws_region"] = LogicalType::VARCHAR;
	function.named_parameters["token"] = LogicalType::VARCHAR;
}

unique_ptr<SecretEntry> GetSecret(ClientContext &context, const string &secret_name) {
	auto &secret_manager = SecretManager::Get(context);
	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
	// FIXME: this should be adjusted once the `GetSecretByName` API supports this
	// use case
	auto secret_entry = secret_manager.GetSecretByName(transaction, secret_name, "memory");
	if (secret_entry) {
		return secret_entry;
	}
	secret_entry = secret_manager.GetSecretByName(transaction, secret_name, "local_file");
	if (secret_entry) {
		return secret_entry;
	}
	return nullptr;
}

static unique_ptr<Catalog> IcebergCatalogAttach(StorageExtensionInfo *storage_info, ClientContext &context,
                                           AttachedDatabase &db, const string &name, AttachInfo &info,
                                           AccessMode access_mode) {
	IRCCredentials credentials;

	string account_id;
	string catalog;

	// check if we have a secret provided
	string secret_name;
	for (auto &entry : info.options) {
		auto lower_name = StringUtil::Lower(entry.first);
		if (lower_name == "type" || lower_name == "read_only") {
			// already handled
		} else if (lower_name == "credentials") {
			secret_name = entry.second.ToString();
		} else if (lower_name == "account_id") {
			account_id = entry.second.ToString();
		} else if (lower_name == "catalog") {
			catalog = StringUtil::Lower(entry.second.ToString());
		} else {
			throw BinderException("Unrecognized option for PC attach: %s", entry.first);
		}
	}

	if (catalog == "s3_tables_glue") {
		// if no iceberg secret is specified we default to the unnamed mysql secret, if it
		// exists
		bool explicit_secret = !secret_name.empty();
		if (explicit_secret) {
			// Lookup explicit secret
			auto secret_entry = GetSecret(context, secret_name);
			if (secret_entry) {
				const auto &kv_secret = dynamic_cast<const KeyValueSecret &>(*secret_entry->secret);
				credentials.aws_region = kv_secret.TryGetValue("region").ToString();
				credentials.endpoint = "https://glue." + credentials.aws_region +  ".amazonaws.com/iceberg/v1";
			} else {
				throw BinderException("Secret with name \"%s\" not found", secret_name);
			}
		} else {
			// look up any s3 secret
			auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
			auto secret_match = context.db->GetSecretManager().LookupSecret(transaction, "s3://", "s3");
			if (!secret_match.HasMatch()) {
				throw IOException("Failed to find a secret and no explicit secret was passed!");
			}
			auto kv_secret = dynamic_cast<const KeyValueSecret &>(secret_match.GetSecret());
			credentials.aws_region = kv_secret.TryGetValue("region").ToString();
			credentials.endpoint = "https://glue." + credentials.aws_region +  ".amazonaws.com/iceberg/v1";
		}

		string path = "/catalogs/" + account_id + ":s3tablescatalog:" + info.path + "/";

		return make_uniq<IRCatalog>(db, path, access_mode, credentials);
	}

	// Default IRC path

	// if no iceberg secret is specified we default to the unnamed mysql secret, if it
	// exists
	bool explicit_secret = !secret_name.empty();
	if (!explicit_secret) {
		// look up settings from the default unnamed mysql secret if none is
		// provided
		secret_name = "__default_iceberg";
	}

	string connection_string = info.path;
	auto secret_entry = GetSecret(context, secret_name);
	if (secret_entry) {
		// secret found - read data
		const auto &kv_secret = dynamic_cast<const KeyValueSecret &>(*secret_entry->secret);
		string new_connection_info;

		Value token_val = kv_secret.TryGetValue("token");
		if (token_val.IsNull()) {
			throw std::runtime_error("Token is blank");
		}
		credentials.token = token_val.ToString();

		Value endpoint_val = kv_secret.TryGetValue("endpoint");
		credentials.endpoint = endpoint_val.IsNull() ? "" : endpoint_val.ToString();
		StringUtil::RTrim(credentials.endpoint, "/");

		Value aws_region_val = kv_secret.TryGetValue("aws_region");
		credentials.aws_region = endpoint_val.IsNull() ? "" : aws_region_val.ToString();

	} else if (explicit_secret) {
		// secret not found and one was explicitly provided - throw an error
		throw BinderException("Secret with name \"%s\" not found", secret_name);
	}

	// TODO: Check catalog with name actually exists!

	return make_uniq<IRCatalog>(db, info.path, access_mode, credentials);
}

static unique_ptr<TransactionManager> CreateTransactionManager(StorageExtensionInfo *storage_info, AttachedDatabase &db,
                                                               Catalog &catalog) {
	auto &ic_catalog = catalog.Cast<IRCatalog>();
	return make_uniq<ICTransactionManager>(db, ic_catalog);
}

class IRCStorageExtension : public StorageExtension {
public:
	IRCStorageExtension() {
		attach = IcebergCatalogAttach;
		create_transaction_manager = CreateTransactionManager;
	}
};

static void LoadInternal(DatabaseInstance &instance) {
	Aws::SDKOptions options;
	Aws::InitAPI(options); // Should only be called once.

	auto &config = DBConfig::GetConfig(instance);

	config.AddExtensionOption(
		"unsafe_enable_version_guessing",
		"Enable globbing the filesystem (if possible) to find the latest version metadata. This could result in reading an uncommitted version.",
		LogicalType::BOOLEAN,
		Value::BOOLEAN(false)
	);

	// Iceberg Table Functions
	for (auto &fun : IcebergFunctions::GetTableFunctions()) {
		ExtensionUtil::RegisterFunction(instance, fun);
	}

	// Iceberg Scalar Functions
	for (auto &fun : IcebergFunctions::GetScalarFunctions()) {
		ExtensionUtil::RegisterFunction(instance, fun);
	}

	IRCAPI::InitializeCurl();

	SecretType secret_type;
	secret_type.name = "iceberg";
	secret_type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
	secret_type.default_provider = "config";

	ExtensionUtil::RegisterSecretType(instance, secret_type);
	CreateSecretFunction secret_function = {"iceberg", "config", CreateCatalogSecretFunction};
	SetCatalogSecretParameters(secret_function);
	ExtensionUtil::RegisterFunction(instance, secret_function);

	config.storage_extensions["iceberg"] = make_uniq<IRCStorageExtension>();
}

void IcebergExtension::Load(DuckDB &db) {
	LoadInternal(*db.instance);
}
std::string IcebergExtension::Name() {
	return "iceberg";
}

} // namespace duckdb

extern "C" {

DUCKDB_EXTENSION_API void iceberg_init(duckdb::DatabaseInstance &db) {
	LoadInternal(db);
}

DUCKDB_EXTENSION_API const char *iceberg_version() {
	return duckdb::DuckDB::LibraryVersion();
}
}

#ifndef DUCKDB_EXTENSION_MAIN
#error DUCKDB_EXTENSION_MAIN not defined
#endif
