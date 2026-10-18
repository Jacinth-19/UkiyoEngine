// Joint matrices for skinned meshes (up to 128 joints).
UBO(2, 9) SkinBlock {
    mat4 joints[128];
} skin;
