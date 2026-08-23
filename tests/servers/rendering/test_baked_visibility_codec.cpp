/**************************************************************************/
/*  test_baked_visibility_codec.cpp                                      */
/**************************************************************************/

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_baked_visibility_codec)

#include "scene/resources/3d/baked_visibility_data_3d.h"
#include "servers/rendering/baked_visibility/baked_visibility_codec.h"

#include <limits>

namespace TestBakedVisibilityCodec {

static BakedVisibilityData3DData::Instance instance(const char *p_path) {
	BakedVisibilityData3DData::Instance value;
	value.path = NodePath(p_path);
	value.local_bounds = AABB(Vector3(), Vector3(1, 1, 1));
	value.signature_sha256.resize(32);
	for (int i = 0; i < 32; i++) {
		value.signature_sha256.write[i] = uint8_t(i + p_path[0]);
	}
	return value;
}

static BakedVisibilityData3DData data() {
	BakedVisibilityData3DData value;
	value.source_uid = 1;
	value.source_path = "res://scene.tscn";
	value.source_sha256.resize(32);
	value.local_bounds = AABB(Vector3(), Vector3(2, 1, 1));
	value.cell_size = Vector3(1, 1, 1);
	value.grid_size = Vector3i(2, 1, 1);
	value.instances.push_back(instance("B"));
	value.instances.push_back(instance("A"));
	value.sets.push_back(Vector<uint32_t>());
	value.sets.push_back(Vector<uint32_t>{ 0 });
	value.sets.push_back(Vector<uint32_t>{ 0, 1 });
	value.cells.resize(2);
	value.cells.write[0].primary_set = 1;
	value.cells.write[0].transport_set = 2;
	value.cells.write[1].primary_set = 0;
	value.cells.write[1].transport_set = 1;
	value.report = "completed";
	value.stage_times_ms.push_back(1.25);
	return value;
}

TEST_CASE("[Rendering][BakedVisibility] codec save/load is equivalent") {
	PackedByteArray bytes;
	String error;
	CHECK_EQ(BakedVisibilityCodec::encode(data(), bytes, &error), OK);
	BakedVisibilityData3DData decoded;
	CHECK_EQ(BakedVisibilityCodec::decode(bytes, decoded, &error), OK);
	CHECK_EQ(decoded.source_path, "res://scene.tscn");
	CHECK_EQ(decoded.instances.size(), 2);
	CHECK_EQ(String(decoded.instances[0].path), "A");
	CHECK_EQ(String(decoded.instances[1].path), "B");
	CHECK_EQ(decoded.cells.size(), 2);
	CHECK_EQ(decoded.stage_times_ms[0], doctest::Approx(1.25));
}

TEST_CASE("[Rendering][BakedVisibility] codec rejects corruption and unsupported versions") {
	PackedByteArray bytes;
	CHECK_EQ(BakedVisibilityCodec::encode(data(), bytes), OK);
	CHECK_EQ(BakedVisibilityData3DData::ALGORITHM_VERSION, 1);
	CHECK_EQ(bytes[8], uint8_t(BakedVisibilityData3DData::ALGORITHM_VERSION & 0xff));
	CHECK_EQ(bytes[9], uint8_t((BakedVisibilityData3DData::ALGORITHM_VERSION >> 8) & 0xff));
	CHECK_EQ(bytes[10], uint8_t((BakedVisibilityData3DData::ALGORITHM_VERSION >> 16) & 0xff));
	CHECK_EQ(bytes[11], uint8_t(BakedVisibilityData3DData::ALGORITHM_VERSION >> 24));
	bytes.write[bytes.size() - 1] ^= 1;
	BakedVisibilityData3DData decoded;
	CHECK_EQ(BakedVisibilityCodec::decode(bytes, decoded), ERR_FILE_CORRUPT);
	CHECK_EQ(BakedVisibilityCodec::encode(data(), bytes), OK);
	bytes.write[4] = 2;
	CHECK_EQ(BakedVisibilityCodec::decode(bytes, decoded), ERR_FILE_UNRECOGNIZED);
	CHECK_EQ(BakedVisibilityCodec::encode(data(), bytes), OK);
	bytes.write[8] = uint8_t(BakedVisibilityData3DData::ALGORITHM_VERSION + 1);
	CHECK_EQ(BakedVisibilityCodec::decode(bytes, decoded), ERR_FILE_UNRECOGNIZED);
	Ref<BakedVisibilityData3D> resource;
	resource.instantiate();
	resource->set_payload(bytes);
	CHECK_FALSE(resource->is_valid());
	CHECK(resource->get_baked_data() == nullptr);
}

TEST_CASE("[Rendering][BakedVisibility] codec rejects malformed paths, flags, and stage timings") {
	BakedVisibilityData3DData malformed = data();
	malformed.source_path = "scene.tscn";
	CHECK_EQ(BakedVisibilityCodec::validate(malformed), ERR_INVALID_DATA);
	malformed = data();
	malformed.instances.write[0].path = NodePath("../Outside");
	PackedByteArray bytes;
	CHECK_EQ(BakedVisibilityCodec::encode(malformed, bytes), ERR_INVALID_DATA);
	malformed = data();
	malformed.instances.write[0].path = NodePath("/Root/Outside");
	CHECK_EQ(BakedVisibilityCodec::validate(malformed), ERR_INVALID_DATA);
	malformed = data();
	malformed.instances.write[0].flags = 1u << 31;
	CHECK_EQ(BakedVisibilityCodec::validate(malformed), ERR_INVALID_DATA);
	malformed = data();
	malformed.cells.write[0].flags = 1u << 31;
	CHECK_EQ(BakedVisibilityCodec::validate(malformed), ERR_INVALID_DATA);
	malformed = data();
	malformed.stage_times_ms.write[0] = -0.01;
	CHECK_EQ(BakedVisibilityCodec::validate(malformed), ERR_INVALID_DATA);
	malformed = data();
	malformed.stage_times_ms.write[0] = std::numeric_limits<double>::infinity();
	CHECK_EQ(BakedVisibilityCodec::validate(malformed), ERR_INVALID_DATA);
}

TEST_CASE("[Rendering][BakedVisibility] resource retains only a validated decoded payload") {
	Ref<BakedVisibilityData3D> resource;
	resource.instantiate();
	CHECK_EQ(resource->set_baked_data(data()), OK);
	CHECK(resource->is_valid());
	CHECK(resource->get_baked_data() != nullptr);
	resource->set_payload(PackedByteArray());
	CHECK_FALSE(resource->is_valid());
	CHECK(resource->get_baked_data() == nullptr);
}

TEST_CASE("[Rendering][BakedVisibility] canonical bytes do not depend on input ordering") {
	BakedVisibilityData3DData first = data();
	BakedVisibilityData3DData second = first;
	second.instances.reverse();
	second.sets.clear();
	second.sets.push_back(Vector<uint32_t>{ 0, 1 }); // A, B.
	second.sets.push_back(Vector<uint32_t>());
	second.sets.push_back(Vector<uint32_t>{ 1 }); // B.
	second.cells.write[0].primary_set = 2;
	second.cells.write[0].transport_set = 0;
	second.cells.write[1].primary_set = 1;
	second.cells.write[1].transport_set = 2;
	PackedByteArray first_bytes;
	PackedByteArray second_bytes;
	CHECK_EQ(BakedVisibilityCodec::encode(first, first_bytes), OK);
	CHECK_EQ(BakedVisibilityCodec::encode(second, second_bytes), OK);
	CHECK_EQ(first_bytes, second_bytes);
}

} // namespace TestBakedVisibilityCodec
