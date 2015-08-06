// Copyright 2010-2013 RethinkDB, all rights reserved.

#include <string>
#include <utility>
#include <vector>

#include "rdb_protocol/error.hpp"
#include "rdb_protocol/func.hpp"
#include "rdb_protocol/minidriver.hpp"
#include "rdb_protocol/op.hpp"

namespace ql {

// Use this merge if it should theoretically never be called.
datum_t pure_merge(UNUSED const datum_string_t &key,
                   UNUSED datum_t l,
                   UNUSED datum_t r,
                   UNUSED const configured_limits_t &limits,
                   UNUSED std::set<std::string> *conditions) {
    r_sanity_check(false);
    return datum_t();
}

datum_t new_stats_object() {
    datum_object_builder_t stats;
    const char *const keys[] =
        {"inserted", "deleted", "skipped", "replaced", "unchanged", "errors"};
    for (size_t i = 0; i < sizeof(keys)/sizeof(*keys); ++i) {
        UNUSED bool b = stats.add(keys[i], datum_t(0.0));
    }
    return std::move(stats).to_datum();
}

conflict_behavior_t parse_conflict_optarg(const scoped_ptr_t<val_t> &arg) {
    if (!arg.has()) { return conflict_behavior_t::ERROR; }
    const datum_string_t &str = arg->as_str();
    if (str == "error") { return conflict_behavior_t::ERROR; }
    if (str == "replace") { return conflict_behavior_t::REPLACE; }
    if (str == "update") { return conflict_behavior_t::UPDATE; }
    rfail_target(arg.get(),
                 base_exc_t::LOGIC,
                 "Conflict option `%s` unrecognized "
                 "(options are \"error\", \"replace\" and \"update\").",
                 str.to_std().c_str());
}

durability_requirement_t parse_durability_optarg(const scoped_ptr_t<val_t> &arg) {
    if (!arg.has()) { return DURABILITY_REQUIREMENT_DEFAULT; }
    const datum_string_t &str = arg->as_str();
    if (str == "hard") { return DURABILITY_REQUIREMENT_HARD; }
    if (str == "soft") { return DURABILITY_REQUIREMENT_SOFT; }
    rfail_target(arg.get(),
                 base_exc_t::LOGIC,
                 "Durability option `%s` unrecognized "
                 "(options are \"hard\" and \"soft\").",
                 str.to_std().c_str());
}

return_changes_t parse_return_changes(
    scope_env_t *env, args_t *args, backtrace_id_t bt) {
    if (args->optarg(env, "return_vals")) {
        rfail_src(bt, base_exc_t::LOGIC,
                  "Error: encountered obsolete optarg `return_vals`.  "
                  "Use `return_changes` instead.");
    }
    if (scoped_ptr_t<val_t> v = args->optarg(env, "return_changes")) {
        datum_t d = v->as_datum();
        if (d.get_type() == datum_t::R_STR) {
            rcheck_src(bt, d.as_str() == "always", base_exc_t::LOGIC,
                       strprintf("Invalid return_changes value `%s` "
                                 "(options are `true`, `false`, and `'always'`.)",
                                 d.as_str().to_std().c_str()));
            return return_changes_t::ALWAYS;
        } else {
            return v->as_bool() ? return_changes_t::YES : return_changes_t::NO;
        }
    } else {
        return return_changes_t::NO;
    }
}

class insert_term_t : public op_term_t {
public:
    insert_term_t(compile_env_t *env, const protob_t<const Term> &term)
        : op_term_t(env, term, argspec_t(2),
                    optargspec_t({"conflict", "durability", "return_vals",
                                  "return_changes"})) { }

private:
    static void maybe_generate_key(counted_t<table_t> tbl,
                                   const configured_limits_t &limits,
                                   std::vector<std::string> *generated_keys_out,
                                   size_t *keys_skipped_out,
                                   datum_t *datum_out,
                                   bool *pkey_was_autogenerated_out) {
        if (!(*datum_out).get_field(datum_string_t(tbl->get_pkey()), NOTHROW).has()) {
            std::string key = uuid_to_str(generate_uuid());
            datum_t keyd((datum_string_t(key)));
            {
                datum_object_builder_t d;
                bool conflict = d.add(datum_string_t(tbl->get_pkey()), keyd);
                r_sanity_check(!conflict);
                std::set<std::string> conditions;
                *datum_out = (*datum_out).merge(std::move(d).to_datum(), pure_merge,
                                                 limits, &conditions);
                // we happen to know that pure_merge cannot ever generate warning
                // conditions, because it shouldn't ever be run.
                r_sanity_check(conditions.size() == 0);
            }
            if (generated_keys_out->size() < limits.array_size_limit()) {
                generated_keys_out->push_back(key);
            } else {
                *keys_skipped_out += 1;
            }
            /* NOTE: If we ever support other pkey autogeneration schemes, it's important
            that this be set to `true` only if a regular UUID is generated, and not for
            any other pkey autogeneration scheme. This is because the artificial tables
            will assume that if this is set to `true`, then the pkey is a newly-generated
            UUID. */
            *pkey_was_autogenerated_out = true;
        } else {
            *pkey_was_autogenerated_out = false;
        }
    }

    virtual scoped_ptr_t<val_t>
    eval_impl(scope_env_t *env, args_t *args, eval_flags_t) const {
        counted_t<table_t> t = args->arg(env, 0)->as_table();
        return_changes_t return_changes = parse_return_changes(env, args, backtrace());

        const conflict_behavior_t conflict_behavior
            = parse_conflict_optarg(args->optarg(env, "conflict"));
        const durability_requirement_t durability_requirement
            = parse_durability_optarg(args->optarg(env, "durability"));

        bool done = false;
        datum_t stats = new_stats_object();
        std::vector<std::string> generated_keys;
        std::set<std::string> conditions;
        size_t keys_skipped = 0;
        scoped_ptr_t<val_t> v1 = args->arg(env, 1);
        if (v1->get_type().is_convertible(val_t::type_t::DATUM)) {
            std::vector<datum_t> datums;
            std::vector<bool> pkey_was_autogenerated(1);
            datums.push_back(v1->as_datum());
            if (datums[0].get_type() == datum_t::R_OBJECT) {
                try {
                    bool was_autogenerated;
                    maybe_generate_key(t, env->env->limits(), &generated_keys,
                                       &keys_skipped, &datums[0],
                                       &was_autogenerated);
                    pkey_was_autogenerated[0] = was_autogenerated;
                } catch (const base_exc_t &) {
                    // We just ignore it, the same error will be handled in `replace`.
                    // TODO: that solution sucks.
                }
                datum_t replace_stats = t->batched_insert(
                    env->env, std::move(datums), std::move(pkey_was_autogenerated),
                    conflict_behavior, durability_requirement, return_changes);
                stats = stats.merge(
                    replace_stats, stats_merge, env->env->limits(), &conditions);
                done = true;
            }
        }

        if (!done) {
            counted_t<datum_stream_t> datum_stream = v1->as_seq(env->env);

            batchspec_t batchspec = batchspec_t::user(batch_type_t::TERMINAL, env->env);
            for (;;) {
                std::vector<datum_t> datums
                    = datum_stream->next_batch(env->env, batchspec);
                if (datums.empty()) {
                    break;
                }
                std::vector<bool> pkey_was_autogenerated(datums.size());

                for (size_t i = 0; i < datums.size(); ++i) {
                    try {
                        bool was_autogenerated;
                        maybe_generate_key(t, env->env->limits(), &generated_keys,
                                           &keys_skipped, &datums[i],
                                           &was_autogenerated);
                        pkey_was_autogenerated[i] = was_autogenerated;
                    } catch (const base_exc_t &) {
                        // We just ignore it, the same error will be handled in
                        // `replace`.  TODO: that solution sucks.
                    }
                }

                datum_t replace_stats = t->batched_insert(
                    env->env, std::move(datums), std::move(pkey_was_autogenerated),
                    conflict_behavior, durability_requirement, return_changes);
                stats = stats.merge(
                    replace_stats, stats_merge, env->env->limits(), &conditions);
            }
        }

        if (generated_keys.size() > 0) {
            std::vector<datum_t> genkeys;
            genkeys.reserve(generated_keys.size());
            for (size_t i = 0; i < generated_keys.size(); ++i) {
                genkeys.push_back(datum_t(datum_string_t(generated_keys[i])));
            }
            datum_object_builder_t d;
            UNUSED bool b = d.add("generated_keys",
                                  datum_t(std::move(genkeys),
                                                        env->env->limits()));
            stats = stats.merge(std::move(d).to_datum(), pure_merge,
                                env->env->limits(), &conditions);
        }

        datum_object_builder_t obj(stats);
        obj.add_warnings(conditions, env->env->limits());
        if (keys_skipped > 0) {
            obj.add_warning(
                strprintf("Too many generated keys (%zu), array truncated to %zu.",
                          keys_skipped + generated_keys.size(),
                          generated_keys.size()).c_str(), env->env->limits());
        }

        return new_val(std::move(obj).to_datum());
    }
    virtual const char *name() const { return "insert"; }
};

class replace_term_t : public op_term_t {
public:
    replace_term_t(compile_env_t *env, const protob_t<const Term> &term)
        : op_term_t(env, term, argspec_t(2),
                    optargspec_t({"non_atomic", "durability",
                                  "return_vals", "return_changes"})) { }

private:
    virtual scoped_ptr_t<val_t> eval_impl(
        scope_env_t *env, args_t *args, eval_flags_t) const {
        bool nondet_ok = false;
        if (scoped_ptr_t<val_t> v = args->optarg(env, "non_atomic")) {
            nondet_ok = v->as_bool();
        }
        return_changes_t return_changes = parse_return_changes(env, args, backtrace());

        const durability_requirement_t durability_requirement
            = parse_durability_optarg(args->optarg(env, "durability"));

        if (!nondet_ok) {
            rcheck(args->arg_is_deterministic(1), base_exc_t::LOGIC,
                   "Could not prove argument deterministic.  "
                   "Maybe you want to use the non_atomic flag?");
        }
        counted_t<const func_t> f = args->arg(env, 1)->as_func(CONSTANT_SHORTCUT);
        if (!nondet_ok) {
            // If this isn't true we should have caught it in the `rcheck` above.
            rassert(f->is_deterministic());
        }

        scoped_ptr_t<val_t> v0 = args->arg(env, 0);
        datum_t stats = new_stats_object();
        std::set<std::string> conditions;
        if (v0->get_type().is_convertible(val_t::type_t::SINGLE_SELECTION)) {
            counted_t<single_selection_t> sel = v0->as_single_selection();
            datum_t replace_stats = sel->replace(
                f, nondet_ok, durability_requirement, return_changes);
            stats = stats.merge(replace_stats, stats_merge, env->env->limits(),
                                &conditions);
        } else {
            counted_t<selection_t> tblrows = v0->as_selection(env->env);
            counted_t<table_t> tbl = tblrows->table;
            counted_t<datum_stream_t> ds = tblrows->seq;

            if (f->is_deterministic()) {
                // Attach a transformation to `ds` to pull out the primary key.
                auto x = pb::dummy_var_t::REPLACE_HELPER_ROW;
                r::reql_t map = r::fun(x, r::expr(x)[tbl->get_pkey()]);
                compile_env_t compile_env((var_visibility_t()));
                func_term_t func_term(&compile_env, map.release_counted());
                var_scope_t var_scope;
                counted_t<const func_t> func = func_term.eval_to_func(var_scope);
                ds->add_transformation(map_wire_func_t(func), backtrace());
            }

            batchspec_t batchspec = batchspec_t::user(batch_type_t::TERMINAL, env->env);
            for (;;) {
                std::vector<datum_t> vals = ds->next_batch(env->env, batchspec);
                if (vals.empty()) {
                    break;
                }

                scoped_ptr_t<std::vector<datum_t> > keys;
                if (!f->is_deterministic()) {
                    keys = make_scoped<std::vector<datum_t> >();
                    keys->reserve(vals.size());
                    for (const auto &val : vals) {
                        keys->push_back(
                            val.get_field(datum_string_t(tbl->get_pkey())));
                    }
                }
                datum_t replace_stats = tbl->batched_replace(
                    env->env, vals, keys.has() ? *keys : vals,
                    f, nondet_ok, durability_requirement, return_changes);
                stats = stats.merge(replace_stats, stats_merge, env->env->limits(),
                                    &conditions);
            }
        }

        datum_object_builder_t obj(stats);
        obj.add_warnings(conditions, env->env->limits());
        return new_val(std::move(obj).to_datum());
    }

    virtual const char *name() const { return "replace"; }
};

// DELETE and UPDATE are in rewrites.hpp

class foreach_term_t : public op_term_t {
public:
    foreach_term_t(compile_env_t *env, const protob_t<const Term> &term)
        : op_term_t(env, term, argspec_t(2)) { }

private:
    virtual scoped_ptr_t<val_t>
    eval_impl(scope_env_t *env, args_t *args, eval_flags_t) const {
        const char *fail_msg = "FOR_EACH expects one or more basic write queries.";

        counted_t<datum_stream_t> ds = args->arg(env, 0)->as_seq(env->env);
        datum_t stats = datum_t::empty_object();
        std::set<std::string> conditions;
        batchspec_t batchspec = batchspec_t::user(batch_type_t::TERMINAL, env->env);
        {
            profile::sampler_t sampler("Evaluating elements in for each.",
                                       env->env->trace);
            counted_t<const func_t> f = args->arg(env, 1)->as_func(CONSTANT_SHORTCUT);
            datum_t row;
            while (row = ds->next(env->env, batchspec), row.has()) {
                scoped_ptr_t<val_t> v = f->call(env->env, row);
                try {
                    datum_t d = v->as_datum();
                    if (d.get_type() == datum_t::R_OBJECT) {
                        stats = stats.merge(d, stats_merge, env->env->limits(),
                                             &conditions);
                    } else {
                        for (size_t i = 0; i < d.arr_size(); ++i) {
                            stats = stats.merge(
                                d.get(i), stats_merge, env->env->limits(), &conditions);
                        }
                    }
                } catch (const exc_t &e) {
                    throw exc_t(e.get_type(), fail_msg, e.backtrace(), e.dummy_frames());
                } catch (const datum_exc_t &de) {
                    rfail_target(v, de.get_type(), "%s  %s", fail_msg, de.what());
                }
                sampler.new_sample();
            }
        }
        datum_object_builder_t obj(stats);
        obj.add_warnings(conditions, env->env->limits());
        return new_val(std::move(obj).to_datum());
    }

    virtual const char *name() const { return "foreach"; }
};

counted_t<term_t> make_insert_term(
        compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<insert_term_t>(env, term);
}

counted_t<term_t> make_replace_term(
        compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<replace_term_t>(env, term);
}

counted_t<term_t> make_foreach_term(
        compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<foreach_term_t>(env, term);
}

} // namespace ql
