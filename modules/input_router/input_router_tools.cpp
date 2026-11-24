// Editor-only: the export baker and the static map checker. Resources in, no router state.

#ifdef TOOLS_ENABLED

#include "input_router.h"
#include "input_router_internal.hpp"

#include "core/templates/local_vector.h"

namespace InputRouterInternal {

static constexpr float CAP_WARN_FRACTION = 0.75f;

struct SourceBinding {
	StringName context;
	Ref<RouteBindingResource> binding;
	String source;
	bool from_map = false;
};

static StringName context_id_of(const Ref<RouteBindingResource> &p_binding, const Ref<GamepadContext> &p_fallback) {
	if (p_binding->get_context().is_valid()) {
		return p_binding->get_context()->get_id();
	}
	return p_fallback.is_valid() ? p_fallback->get_id() : StringName();
}

static void gather(const TypedArray<GamepadMap> &p_maps, const Array &p_placements, LocalVector<SourceBinding> &r_out) {
	for (int m = 0; m < p_maps.size(); m++) {
		const Ref<GamepadMap> map = p_maps[m];
		if (map.is_null()) {
			continue;
		}
		const TypedArray<RouteBindingResource> bindings = map->get_bindings();
		for (int i = 0; i < bindings.size(); i++) {
			const Ref<RouteBindingResource> b = bindings[i];
			if (b.is_valid()) {
				r_out.push_back({ context_id_of(b, map->get_context()), b, map->get_path(), true });
			}
		}
	}
	for (int i = 0; i < p_placements.size(); i++) {
		const Variant v = p_placements[i];
		if (v.get_type() == Variant::DICTIONARY) {
			const Dictionary d = v;
			const Ref<RouteBindingResource> b = d.get("binding", Variant());
			if (b.is_valid()) {
				StringName ctx = d.get("context", StringName());
				r_out.push_back({ ctx == StringName() ? context_id_of(b, Ref<GamepadContext>()) : ctx, b, d.get("source", String()), false });
			}
		} else {
			const Ref<RouteBindingResource> b = v;
			if (b.is_valid()) {
				r_out.push_back({ context_id_of(b, Ref<GamepadContext>()), b, b->get_path(), false });
			}
		}
	}
}

static int find_id(const LocalVector<StringName> &p_ids, const StringName &p_id) {
	for (uint32_t i = 0; i < p_ids.size(); i++) {
		if (p_ids[i] == p_id) {
			return i;
		}
	}
	return -1;
}

static bool schemes_overlap(const StringName &p_a, const StringName &p_b) {
	return p_a == StringName() || p_b == StringName() || p_a == p_b;
}

static Dictionary finding(const char *p_rule, const char *p_severity, const StringName &p_context, const String &p_a, const String &p_b, const String &p_message) {
	Dictionary d;
	d["rule"] = p_rule;
	d["severity"] = p_severity;
	d["context"] = p_context;
	d["a"] = p_a;
	d["b"] = p_b;
	d["message"] = p_message;
	return d;
}

// One template per (id, scheme), every context, every distinct curve, built the way the router
// merges a missing binding. Any error fails the export.
Ref<GamepadBakedMaps> bake(const TypedArray<GamepadContext> &p_contexts, const TypedArray<GamepadMap> &p_maps, const TypedArray<RouteBindingResource> &p_bindings, PackedStringArray &r_errors) {
	LocalVector<SourceBinding> sources;
	gather(p_maps, Array(p_bindings), sources);

	Dictionary data = Baked::empty();
	bool contexts_full = false;
	for (int i = 0; i < p_contexts.size(); i++) {
		const Ref<GamepadContext> c = p_contexts[i];
		if (c.is_valid() && c->get_id() != StringName()) {
			contexts_full = !Baked::add_context(data, c->get_id(), c->get_fallback(), c->is_modal()) || contexts_full;
		}
	}

	for (const SourceBinding &sb : sources) {
		const Ref<RouteBindingResource> &b = sb.binding;
		String error;
		const Baked::Added added = Baked::add_binding(data, **b, error);
		if (added == Baked::ADD_FAILED) {
			r_errors.push_back(vformat("%s: binding '%s': %s.", sb.source, b->get_id(), error));
			continue;
		}
		if (added == Baked::ADD_REPLACED) {
			r_errors.push_back(vformat("%s: id '%s' is used twice with different content.", sb.source, b->get_id()));
		}
		if (sb.context == StringName()) {
			if (sb.from_map) {
				r_errors.push_back(vformat("%s: binding '%s' has no context.", sb.source, b->get_id()));
			}
			continue;
		}
		// A placement context the list lacks inherits; only the binding's own context brings its flags.
		contexts_full = !Baked::add_context(data, sb.context, GamepadContext::FALLBACK_INHERIT, false) || contexts_full;
	}
	if (contexts_full) {
		r_errors.push_back(vformat("More than %d contexts.", MAX_CONTEXTS));
	}

	Ref<GamepadBakedMaps> baked;
	baked.instantiate();
	baked->set_data(data);
	return baked;
}

// Runtime routing's clash rules, on data only. p_placements: RouteBindingResources or
// {context, binding, source}. p_paths: context paths seen, outer to inner. Rules: missing_id,
// compile_error, duplicate_id, unknown_context, clash, gate_bound_alone, shadowed, cap.
TypedArray<Dictionary> check_maps(const TypedArray<GamepadContext> &p_contexts, const TypedArray<GamepadMap> &p_maps, const Array &p_placements, const TypedArray<PackedStringArray> &p_paths) {
	TypedArray<Dictionary> result;
	LocalVector<SourceBinding> sources;
	gather(p_maps, p_placements, sources);

	LocalVector<StringName> known;
	LocalVector<bool> modal;
	for (int i = 0; i < p_contexts.size(); i++) {
		const Ref<GamepadContext> c = p_contexts[i];
		if (c.is_valid()) {
			known.push_back(c->get_id());
			modal.push_back(c->is_modal());
		}
	}

	LocalVector<RouteBinding> rows;
	LocalVector<bool> valid;
	for (const SourceBinding &sb : sources) {
		RouteBinding row;
		float lut[LUT_STRIDE];
		bool has_lut = false;
		String error;
		const bool ok = compile_binding(**sb.binding, row, lut, has_lut, error);
		rows.push_back(row);
		valid.push_back(ok && sb.binding->get_id() != StringName());
		const String id = sb.binding->get_id();
		if (id.is_empty()) {
			result.push_back(finding("missing_id", "error", sb.context, sb.source, String(), "Binding has no id."));
		}
		if (!ok) {
			result.push_back(finding("compile_error", "error", sb.context, id, String(), error));
		}
		if (sb.context == StringName()) {
			result.push_back(finding("unknown_context", "error", sb.context, id, String(), vformat("%s: binding has no context.", sb.source)));
		} else if (find_id(known, sb.context) < 0) {
			result.push_back(finding("unknown_context", "warning", sb.context, id, String(), vformat("%s: context '%s' is not in the context list.", sb.source, sb.context)));
		}
	}

	uint32_t unique = 0;
	for (uint32_t i = 0; i < sources.size(); i++) {
		if (!valid[i]) {
			continue;
		}
		const RouteBindingResource &a = **sources[i].binding;
		bool first = true;
		for (uint32_t j = 0; j < sources.size(); j++) {
			if (!valid[j] || i == j) {
				continue;
			}
			const RouteBindingResource &b = **sources[j].binding;
			const bool same_id = a.get_id() == b.get_id() && a.get_scheme() == b.get_scheme();
			if (same_id && j < i) {
				first = false;
			}
			if (j < i) {
				continue;
			}
			const bool same_context = sources[i].context == sources[j].context;
			if (same_id && sources[i].binding != sources[j].binding && (!rows[i].same_tuning(rows[j]) || a.get_action() != b.get_action())) {
				result.push_back(finding("duplicate_id", "error", sources[i].context, a.get_id(), sources[j].source, "Same id and scheme, different content."));
			}
			const bool routed = rows[i].trigger != RouteBindingResource::TRIGGER_SEQUENCE && rows[j].trigger != RouteBindingResource::TRIGGER_SEQUENCE;
			if (!same_id && same_context && routed && rows[i].input == rows[j].input && rows[i].gate == rows[j].gate && rows[i].priority == rows[j].priority && schemes_overlap(a.get_scheme(), b.get_scheme())) {
				result.push_back(finding("clash", "error", sources[i].context, a.get_id(), b.get_id(), vformat("Both claim %s; set a priority or a scheme.", input_label(rows[i].input, InputRouter::PAD_FAMILY_GENERIC))));
			}
			if (same_context && rows[j].gate != NONE && rows[i].gate == NONE && rows[i].input == rows[j].gate) {
				result.push_back(finding("gate_bound_alone", "warning", sources[i].context, a.get_id(), b.get_id(), vformat("%s is a chord gate here; bound alone it fires on release only.", input_label(rows[i].input, InputRouter::PAD_FAMILY_GENERIC))));
			}
			if (same_context && rows[i].gate != NONE && rows[j].gate == NONE && rows[j].input == rows[i].gate) {
				result.push_back(finding("gate_bound_alone", "warning", sources[i].context, b.get_id(), a.get_id(), vformat("%s is a chord gate here; bound alone it fires on release only.", input_label(rows[j].input, InputRouter::PAD_FAMILY_GENERIC))));
			}
		}
		if (first) {
			unique++;
		}
	}

	for (int p = 0; p < p_paths.size(); p++) {
		const PackedStringArray path = p_paths[p];
		int cut = 0;
		for (int i = path.size() - 1; i >= 0; i--) {
			const int c = find_id(known, path[i]);
			if (c >= 0 && modal[c]) {
				cut = i;
				break;
			}
		}
		for (uint32_t i = 0; i < sources.size(); i++) {
			const int depth = path.find(sources[i].context);
			if (!valid[i] || depth < 0) {
				continue;
			}
			if (depth < cut) {
				result.push_back(finding("shadowed", "info", sources[i].context, sources[i].binding->get_id(), path[cut], vformat("Below modal '%s' on path %s.", path[cut], String(", ").join(path))));
				continue;
			}
			for (uint32_t j = 0; j < sources.size(); j++) {
				const int inner = path.find(sources[j].context);
				if (!valid[j] || inner <= depth || rows[j].input != rows[i].input || rows[j].gate != NONE || rows[j].trigger == RouteBindingResource::TRIGGER_SEQUENCE || !schemes_overlap(sources[i].binding->get_scheme(), sources[j].binding->get_scheme())) {
					continue;
				}
				result.push_back(finding("shadowed", "info", sources[i].context, sources[i].binding->get_id(), sources[j].binding->get_id(), vformat("Hidden by '%s' in '%s' on path %s.", sources[j].binding->get_id(), sources[j].context, String(", ").join(path))));
				break;
			}
		}
		if (path.size() > MAX_PATH_DEPTH * CAP_WARN_FRACTION) {
			result.push_back(finding("cap", "warning", StringName(), "path", String(), vformat("Path of %d contexts; the cap is %d.", path.size(), MAX_PATH_DEPTH)));
		}
	}

	struct Cap {
		const char *name;
		uint32_t count;
		uint32_t cap;
	};
	const Cap caps[] = {
		{ "contexts", known.size(), MAX_CONTEXTS },
		{ "bindings", unique, MAX_BAKED },
		{ "attached bindings", sources.size(), MAX_ROWS },
	};
	for (const Cap &c : caps) {
		if (c.count > c.cap * CAP_WARN_FRACTION) {
			result.push_back(finding("cap", c.count > c.cap ? "error" : "warning", StringName(), c.name, String(), vformat("%d %s; the cap is %d.", c.count, c.name, c.cap)));
		}
	}
	return result;
}

} // namespace InputRouterInternal

#endif // TOOLS_ENABLED
