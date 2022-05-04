/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2020-present ScyllaDB
 */

#include <any>
#include <seastar/core/sstring.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/smp.hh>
#include "log.hh"
#include "db/config.hh"
#include "gms/feature.hh"
#include "gms/feature_service.hh"
#include "db/system_keyspace.hh"
#include "db/query_context.hh"
#include "to_string.hh"
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>

using namespace std::literals;

namespace gms {

static logging::logger logger("features");

feature_config::feature_config() {
}

feature_service::feature_service(feature_config cfg) : _config(cfg)
        , _udf_feature(*this, "UDF"sv)
        , _md_sstable_feature(*this, "MD_SSTABLE_FORMAT"sv)
        , _me_sstable_feature(*this, "ME_SSTABLE_FORMAT"sv)
        , _view_virtual_columns(*this, "VIEW_VIRTUAL_COLUMNS"sv)
        , _digest_insensitive_to_expiry(*this, "DIGEST_INSENSITIVE_TO_EXPIRY"sv)
        , _computed_columns(*this, "COMPUTED_COLUMNS"sv)
        , _cdc_feature(*this, "CDC"sv)
        , _nonfrozen_udts(*this, "NONFROZEN_UDTS"sv)
        , _hinted_handoff_separate_connection(*this, "HINTED_HANDOFF_SEPARATE_CONNECTION"sv)
        , _lwt_feature(*this, "LWT"sv)
        , _per_table_partitioners_feature(*this, "PER_TABLE_PARTITIONERS"sv)
        , _per_table_caching_feature(*this, "PER_TABLE_CACHING"sv)
        , _digest_for_null_values_feature(*this, "DIGEST_FOR_NULL_VALUES"sv)
        , _correct_idx_token_in_secondary_index_feature(*this, "CORRECT_IDX_TOKEN_IN_SECONDARY_INDEX"sv)
        , _alternator_streams_feature(*this, "ALTERNATOR_STREAMS"sv)
        , _alternator_ttl_feature(*this, "ALTERNATOR_TTL"sv)
        , _range_scan_data_variant(*this, "RANGE_SCAN_DATA_VARIANT"sv)
        , _cdc_generations_v2(*this, "CDC_GENERATIONS_V2"sv)
        , _uda(*this, "UDA"sv)
        , _separate_page_size_and_safety_limit(*this, "SEPARATE_PAGE_SIZE_AND_SAFETY_LIMIT"sv)
        , _supports_raft_cluster_mgmt(*this, "SUPPORTS_RAFT_CLUSTER_MANAGEMENT"sv)
        , _uses_raft_cluster_mgmt(*this, "USES_RAFT_CLUSTER_MANAGEMENT"sv)
        , _tombstone_gc_options(*this, "TOMBSTONE_GC_OPTIONS"sv)
        , _parallelized_aggregation(*this, "PARALLELIZED_AGGREGATION"sv)
        , _keyspace_storage_options(*this, "KEYSPACE_STORAGE_OPTIONS"sv)
{}

feature_config feature_config_from_db_config(db::config& cfg, std::set<sstring> disabled) {
    feature_config fcfg;

    fcfg._disabled_features = std::move(disabled);

    switch (sstables::from_string(cfg.sstable_format())) {
    case sstables::sstable_version_types::ka:
    case sstables::sstable_version_types::la:
    case sstables::sstable_version_types::mc:
        fcfg._disabled_features.insert("MD_SSTABLE_FORMAT"s);
        [[fallthrough]];
    case sstables::sstable_version_types::md:
        fcfg._disabled_features.insert("ME_SSTABLE_FORMAT"s);
        [[fallthrough]];
    case sstables::sstable_version_types::me:
        break;
    }

    if (!cfg.enable_user_defined_functions()) {
        fcfg._disabled_features.insert("UDF");
    } else {
        if (!cfg.check_experimental(db::experimental_features_t::UDF)) {
            throw std::runtime_error(
                    "You must use both enable_user_defined_functions and experimental_features:udf "
                    "to enable user-defined functions");
        }
    }

    if (!cfg.check_experimental(db::experimental_features_t::ALTERNATOR_STREAMS)) {
        fcfg._disabled_features.insert("ALTERNATOR_STREAMS"s);
    }
    if (!cfg.check_experimental(db::experimental_features_t::ALTERNATOR_TTL)) {
        fcfg._disabled_features.insert("ALTERNATOR_TTL"s);
    }
    if (!cfg.check_experimental(db::experimental_features_t::RAFT)) {
        fcfg._disabled_features.insert("SUPPORTS_RAFT_CLUSTER_MANAGEMENT"s);
        fcfg._disabled_features.insert("USES_RAFT_CLUSTER_MANAGEMENT"s);
    } else {
        // Disable support for using raft cluster management so that it cannot
        // be enabled by accident.
        // This prevents the `USES_RAFT_CLUSTER_MANAGEMENT` feature from being
        // advertised via gossip ahead of time.
        fcfg._masked_features.insert("USES_RAFT_CLUSTER_MANAGEMENT"s);
    }
    if (!cfg.check_experimental(db::experimental_features_t::KEYSPACE_STORAGE_OPTIONS)) {
        fcfg._disabled_features.insert("KEYSPACE_STORAGE_OPTIONS"s);
    }

    return fcfg;
}

future<> feature_service::stop() {
    return make_ready_future<>();
}

void feature_service::register_feature(feature& f) {
    auto i = _registered_features.emplace(f.name(), f);
    assert(i.second);
}

void feature_service::unregister_feature(feature& f) {
    _registered_features.erase(f.name());
}


void feature_service::enable(const sstring& name) {
    if (auto it = _registered_features.find(name); it != _registered_features.end()) {
        auto&& f = it->second;
        auto& f_ref = f.get();
        if (db::qctx && !f_ref) {
            persist_enabled_feature_info(f_ref);
        }
        f_ref.enable();
    }
}

future<> feature_service::support(const std::string_view& name) {
    _config._masked_features.erase(sstring(name));

    if (db::qctx) {
        // Update `system.local#supported_features` accordingly
        co_await db::system_keyspace::save_local_supported_features(supported_feature_set());
    }
}

std::set<std::string_view> feature_service::known_feature_set() {
    // Add features known by this local node. When a new feature is
    // introduced in scylla, update it here, e.g.,
    // return sstring("FEATURE1,FEATURE2")
    std::set<std::string_view> features = {
        // Deprecated features - sent to other nodes via gossip, but assumed true in the code
        "RANGE_TOMBSTONES"sv,
        "LARGE_PARTITIONS"sv,
        "COUNTERS"sv,
        "DIGEST_MULTIPARTITION_READ"sv,
        "CORRECT_COUNTER_ORDER"sv,
        "SCHEMA_TABLES_V3"sv,
        "CORRECT_NON_COMPOUND_RANGE_TOMBSTONES"sv,
        "WRITE_FAILURE_REPLY"sv,
        "XXHASH"sv,
        "ROLES"sv,
        "LA_SSTABLE_FORMAT"sv,
        "STREAM_WITH_RPC_STREAM"sv,
        "MATERIALIZED_VIEWS"sv,
        "INDEXES"sv,
        "ROW_LEVEL_REPAIR"sv,
        "TRUNCATION_TABLE"sv,
        "CORRECT_STATIC_COMPACT_IN_MC"sv,
        "UNBOUNDED_RANGE_TOMBSTONES"sv,
        "MC_SSTABLE_FORMAT"sv,
    };

    for (auto& [name, f_ref] : _registered_features) {
        features.insert(name);
    }

    for (const sstring& s : _config._disabled_features) {
        features.erase(s);
    }
    return features;
}

const std::unordered_map<sstring, std::reference_wrapper<feature>>& feature_service::registered_features() const {
    return _registered_features;
}

std::set<std::string_view> feature_service::supported_feature_set() {
    auto features = known_feature_set();

    for (const sstring& s : _config._masked_features) {
        features.erase(s);
    }
    return features;
}

feature::feature(feature_service& service, std::string_view name, bool enabled)
        : _service(&service)
        , _name(name)
        , _enabled(enabled) {
    _service->register_feature(*this);
}

feature::~feature() {
    if (_service) {
        _service->unregister_feature(*this);
    }
}

feature& feature::operator=(feature&& other) {
    _service->unregister_feature(*this);
    _service = std::exchange(other._service, nullptr);
    _name = other._name;
    _enabled = other._enabled;
    _s = std::move(other._s);
    _service->register_feature(*this);
    return *this;
}

void feature::enable() {
    if (!_enabled) {
        if (this_shard_id() == 0) {
            logger.info("Feature {} is enabled", name());
        }
        _enabled = true;
        _s();
    }
}

db::schema_features feature_service::cluster_schema_features() const {
    db::schema_features f;
    f.set_if<db::schema_feature::VIEW_VIRTUAL_COLUMNS>(bool(_view_virtual_columns));
    f.set_if<db::schema_feature::DIGEST_INSENSITIVE_TO_EXPIRY>(bool(_digest_insensitive_to_expiry));
    f.set_if<db::schema_feature::COMPUTED_COLUMNS>(bool(_computed_columns));
    f.set_if<db::schema_feature::CDC_OPTIONS>(bool(_cdc_feature));
    f.set_if<db::schema_feature::PER_TABLE_PARTITIONERS>(bool(_per_table_partitioners_feature));
    f.set_if<db::schema_feature::SCYLLA_KEYSPACES>(bool(_keyspace_storage_options));
    return f;
}

std::set<sstring> feature_service::to_feature_set(sstring features_string) {
    std::set<sstring> features;
    boost::split(features, features_string, boost::is_any_of(","));
    features.erase("");
    return features;
}

void feature_service::persist_enabled_feature_info(const gms::feature& f) const {
    // Executed in seastar::async context, because `gms::feature::enable`
    // is only allowed to run within a thread context

    std::optional<sstring> raw_old_value = db::system_keyspace::get_scylla_local_param(ENABLED_FEATURES_KEY).get0();
    if (!raw_old_value) {
        db::system_keyspace::set_scylla_local_param(ENABLED_FEATURES_KEY, f.name()).get0();
        return;
    }
    auto feats_set = to_feature_set(*raw_old_value);
    feats_set.emplace(f.name());
    db::system_keyspace::set_scylla_local_param(ENABLED_FEATURES_KEY, ::join(",", feats_set)).get0();
}

void feature_service::enable(const std::set<std::string_view>& list) {
    for (gms::feature& f : {
        std::ref(_udf_feature),
        std::ref(_md_sstable_feature),
        std::ref(_me_sstable_feature),
        std::ref(_view_virtual_columns),
        std::ref(_digest_insensitive_to_expiry),
        std::ref(_computed_columns),
        std::ref(_cdc_feature),
        std::ref(_nonfrozen_udts),
        std::ref(_hinted_handoff_separate_connection),
        std::ref(_lwt_feature),
        std::ref(_per_table_partitioners_feature),
        std::ref(_per_table_caching_feature),
        std::ref(_digest_for_null_values_feature),
        std::ref(_correct_idx_token_in_secondary_index_feature),
        std::ref(_alternator_streams_feature),
        std::ref(_alternator_ttl_feature),
        std::ref(_range_scan_data_variant),
        std::ref(_cdc_generations_v2),
        std::ref(_uda),
        std::ref(_separate_page_size_and_safety_limit),
        std::ref(_supports_raft_cluster_mgmt),
        std::ref(_uses_raft_cluster_mgmt),
        std::ref(_tombstone_gc_options),
        std::ref(_parallelized_aggregation),
        std::ref(_keyspace_storage_options),
    })
    {
        if (list.contains(f.name())) {
            if (db::qctx && !f) {
                persist_enabled_feature_info(f);
            }
            f.enable();
        }
    }
}

} // namespace gms
