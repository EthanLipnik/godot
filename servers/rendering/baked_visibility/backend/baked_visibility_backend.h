/**************************************************************************/
/*  baked_visibility_backend.h                                            */
/**************************************************************************/

#pragma once

#include "core/math/aabb.h"
#include "core/string/ustring.h"
#include "core/templates/vector.h"
#include "core/typedefs.h"
#include "core/variant/variant.h"

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

// A certificate query is deliberately independent from the broad discovery
// batch.  Patches are canonical, extracted opaque blocker polygons: triangles
// or convex coplanar merges with no more than four vertices.  Anything outside
// that representation is left ambiguous and is certified by the CPU baker.
struct BakedVisibilityBackendCertificatePatch {
	Vector3 vertices[4];
	Vector3 normal;
	uint32_t blocker_id = 0;
	uint32_t patch_id = 0;
	uint8_t vertex_count = 0;
	int8_t source_side = 0; // +1 front, -1 back, 0 two-sided.
};

struct BakedVisibilityBackendCertificateQuery {
	AABB source_bounds;
	AABB target_bounds;
	uint32_t source_id = 0;
	uint32_t target_id = 0;
	uint32_t candidate_bvh_node_id = UINT32_MAX;
	uint32_t patch_index = 0;
};

enum class BakedVisibilityCertificateResult : uint8_t {
	AMBIGUOUS = 0,
	PROVEN = 1,
};

struct BakedVisibilityBackendCertificateBatchInput {
	Vector<BakedVisibilityBackendCertificatePatch> patches;
	Vector<BakedVisibilityBackendCertificateQuery> queries;
	// A canonical patch table is uploaded once per immutable bake contract and
	// referenced by query.patch_index. The digest is a binary table identity,
	// never a renderer pointer or a textual float serialization.
	PackedByteArray patch_table_digest;
	uint32_t contract_revision = 1;
};

struct BakedVisibilityBackendCertificateBatchOutput {
	Vector<BakedVisibilityCertificateResult> results;
	// Query index is the canonical witness order.  A PROVEN result is only
	// consumed after this index and its patch ID have been range-checked.
	Vector<uint32_t> witness_patch_indices;
	uint32_t dispatch_count = 0;
	uint64_t packet_count = 0; // Each packet contains all 8 x 8 corner pairs.
	bool gpu_executed = false;
	bool validation_mismatch = false;
	String diagnostic;
};

// Acceleration is an optional discovery aid.  The CPU certificate remains the
// authority: a backend that cannot prove a complete conservative certificate
// is reported unsupported and the baker stays on the deterministic CPU path.
class BakedVisibilityBackend {
public:
	static BakedVisibilityBackendCapabilities probe(BakedVisibilityBackendKind p_kind);
	static Error execute_cpu_reference(const BakedVisibilityBackendBatchInput &p_input, BakedVisibilityBackendBatchOutput &r_output, String *r_error = nullptr);
	// The scalar oracle exactly mirrors the conservative convex-patch contract.
	// It is used for unsupported packets and deterministic Metal validation.
	static Error execute_cpu_certificate_reference(const BakedVisibilityBackendCertificateBatchInput &p_input, BakedVisibilityBackendCertificateBatchOutput &r_output, String *r_error = nullptr);
	static Error execute_certificate_batch(BakedVisibilityBackendKind p_kind, const BakedVisibilityBackendCertificateBatchInput &p_input, BakedVisibilityBackendCertificateBatchOutput &r_output, bool p_deterministic_validation = false, String *r_error = nullptr);
	// Explicit parity mode. This is the only path that runs both the selected
	// adapter and the CPU oracle; normal editor bakes must not pay that cost.
	static Error execute_deterministic_validation(BakedVisibilityBackendKind p_kind, const BakedVisibilityBackendBatchInput &p_input, BakedVisibilityBackendBatchOutput &r_output, String *r_error = nullptr);
	// Attempts the requested optional adapter. Unavailable acceleration falls
	// back to the scalar CPU discovery contract; deterministic parity is exposed
	// separately so normal editor bakes do not duplicate the complete batch.
	static Error execute(BakedVisibilityBackendKind p_kind, const BakedVisibilityBackendBatchInput &p_input, BakedVisibilityBackendBatchOutput &r_output, String *r_error = nullptr);
};
