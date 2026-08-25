/**************************************************************************/
/*  baked_visibility_checkpoint.h                                         */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#pragma once

#include "servers/rendering/baked_visibility/baked_visibility_baker.h"

// Checkpoints are deliberately separate from .bvis resources: incomplete
// tiles must never become runtime data. The store is canonical, checksummed,
// atomically replaced, and contains only an immutable bake snapshot.
class BakedVisibilityBakeCheckpointStore {
public:
	static Error save(const String &p_path, const BakedVisibilityBakeCheckpoint &p_checkpoint, String *r_error = nullptr);
	static Error load(const String &p_path, BakedVisibilityBakeCheckpoint &r_checkpoint, String *r_error = nullptr);
};
