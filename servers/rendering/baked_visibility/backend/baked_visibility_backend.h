/**************************************************************************/
/*  baked_visibility_backend.h                                            */
/**************************************************************************/

#pragma once

#include "core/math/aabb.h"
#include "core/string/ustring.h"
#include "core/templates/vector.h"
#include "core/typedefs.h"

enum class BakedVisibilityBackendKind : uint8_t {
	AUTO,
	CPU_REFERENCE,
	VULKAN,
	METAL,
};

struct BakedVisibilityBackendCapabilities {
	BakedVisibilityBackendKind kind = BakedVisibilityBackendKind::CPU_REFERENCE;
	bool available = true;
	bool can_discover_candidates = false;
	bool can_certify_invisibility = false;
	// Discovery acceleration can be available even if hardware ray queries are
	// not. Neither capability is allowed to certify a culled object.
	bool supports_gpu_batches = false;
	bool supports_hardware_ray_queries = false;
	bool executed = false;
	String diagnostic = "CPU reference certification";
};

// The batch contract deliberately carries immutable AABBs rather than scene
// objects or renderer-specific handles. This keeps optional acceleration
// product-neutral and makes CPU/GPU output directly comparable.
struct BakedVisibilityBackendCandidate {
	AABB bounds;
	uint32_t canonical_index = 0;
};

struct BakedVisibilityBackendBlocker {
	AABB bounds;
};

struct BakedVisibilityBackendBatchInput {
	AABB query_bounds;
	Vector<BakedVisibilityBackendCandidate> candidates;
	Vector<BakedVisibilityBackendBlocker> blockers;
};

struct BakedVisibilityBackendBatchOutput {
	// One byte per input candidate. A set bit means a candidate belongs to the
	// broad discovery set; it never means the candidate is safe to exclude.
	Vector<uint8_t> candidate_mask;
	// Indices into BatchInput::candidates, compacted and canonically sorted by
	// candidate canonical_index (then source index).
	Vector<uint32_t> compacted_candidate_indices;
	// A deterministic segment-vs-blocker hint for each candidate. This can
	// prioritize subsequent CPU work but cannot suppress CPU certification.
	Vector<uint8_t> blocker_hit_hints;
	// Hardware queries are exposed separately because triangle hits are more
	// precise than the conservative AABB oracle. They remain hints only.
	Vector<uint8_t> hardware_blocker_hit_hints;
	uint32_t dispatch_count = 0;
	uint32_t ray_query_count = 0;
	bool gpu_executed = false;
	bool hardware_ray_queries_executed = false;
	String diagnostic;
};

// Acceleration is an optional discovery aid.  The CPU certificate remains the
// authority: a backend that cannot prove a complete conservative certificate
// is reported unsupported and the baker stays on the deterministic CPU path.
class BakedVisibilityBackend {
public:
	static BakedVisibilityBackendCapabilities probe(BakedVisibilityBackendKind p_kind);
	static Error execute_cpu_reference(const BakedVisibilityBackendBatchInput &p_input, BakedVisibilityBackendBatchOutput &r_output, String *r_error = nullptr);
	// Attempts the requested optional adapter. It always returns a CPU-semantic
	// result: unavailable acceleration is a truthful CPU fallback, and a GPU
	// result is accepted only after equality with the CPU oracle.
	static Error execute(BakedVisibilityBackendKind p_kind, const BakedVisibilityBackendBatchInput &p_input, BakedVisibilityBackendBatchOutput &r_output, String *r_error = nullptr);
};
