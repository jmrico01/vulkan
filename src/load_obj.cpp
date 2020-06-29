#include "load_obj.h"

#include <km_common/km_os.h>

union FaceIndices
{
    struct
    {
        int pos, uv;
    };
    int values[2];
};

bool StringToObjFaceInds(const_string str, FaceIndices* faceInds)
{
    int numElements;
    bool result = StringToElementArray(str, '/', false, StringToIntBase10, 2, faceInds->values, &numElements);
    return result && numElements == 2;
}

bool LoadObj(const_string filePath, LoadObjResult* result, LinearAllocator* allocator)
{
    result->file = LoadEntireFile(filePath, allocator);
    if (result->file.data == nullptr) {
        return false;
    }

    DynamicArray<Vec3, LinearAllocator> positions(allocator);
    DynamicArray<Vec2, LinearAllocator> uvs(allocator);
    DynamicArray<Vertex, LinearAllocator> vertices(allocator);
    DynamicArray<uint64, LinearAllocator> modelEndVertexInds(allocator);

    string fileString = {
        .size = result->file.size,
        .data = (char*)result->file.data
    };
    string next;
    bool firstModel = true;
    while (true) {
        next = NextSplitElement(&fileString, '\n');
        if (next.size == 0) {
            modelEndVertexInds.Append(vertices.size);
            break;
        }

        if (next.size < 2) continue;

        if (next[0] == 'o' && next[1] == ' ') {
            if (firstModel) {
                firstModel = false;
            }
            else {
                modelEndVertexInds.Append(vertices.size);
            }
        }
        else if (next[0] == 'v' && next[1] == ' ') {
            next.data += 2;
            next.size -= 2;

            Vec3* p = positions.Append();
            int numElements;
            if (!StringToElementArray(next, ' ', false, StringToFloat32, 3, p->e, &numElements)) {
                return false;
            }
            if (numElements != 3) {
                return false;
            }
        }
        else if (next.size > 2 && next[0] == 'v' && next[1] == 't' && next[2] == ' ') {
            next.data += 3;
            next.size -= 3;

            Vec2* uv = uvs.Append();
            int numElements;
            if (!StringToElementArray(next, ' ', false, StringToFloat32, 2, uv->e, &numElements)) {
                return false;
            }
            if (numElements != 2) {
                return false;
            }
        }
        else if (next[0] == 'f' && next[1] == ' ') {
            next.data += 2;
            next.size -= 2;

            FaceIndices indices[4];
            int numElements;
            if (!StringToElementArray(next, ' ', false, StringToObjFaceInds, 4, indices, &numElements)) {
                return false;
            }

            // NOTE obj files store faces in counter-clockwise order, but we want to return clockwise
            if (numElements == 3) {
                const Vec3 pos1 = positions[indices[0].pos - 1];
                const Vec3 pos2 = positions[indices[2].pos - 1];
                const Vec3 pos3 = positions[indices[1].pos - 1];
                const Vec3 normal = CalculateTriangleUnitNormal(pos1, pos2, pos3);

                vertices.Append({ .pos = pos1, .normal = normal, .uv = uvs[indices[0].uv - 1] });
                vertices.Append({ .pos = pos2, .normal = normal, .uv = uvs[indices[2].uv - 1] });
                vertices.Append({ .pos = pos3, .normal = normal, .uv = uvs[indices[1].uv - 1] });
            }
            else if (numElements == 4) {
                const Vec3 pos1 = positions[indices[0].pos - 1];
                const Vec3 pos2 = positions[indices[3].pos - 1];
                const Vec3 pos3 = positions[indices[2].pos - 1];
                const Vec3 pos4 = positions[indices[2].pos - 1];
                const Vec3 pos5 = positions[indices[1].pos - 1];
                const Vec3 pos6 = positions[indices[0].pos - 1];
                const Vec3 normal = CalculateTriangleUnitNormal(pos1, pos2, pos3);

                vertices.Append({ .pos = pos1, .normal = normal, .uv = uvs[indices[0].uv - 1] });
                vertices.Append({ .pos = pos2, .normal = normal, .uv = uvs[indices[3].uv - 1] });
                vertices.Append({ .pos = pos3, .normal = normal, .uv = uvs[indices[2].uv - 1] });
                vertices.Append({ .pos = pos4, .normal = normal, .uv = uvs[indices[2].uv - 1] });
                vertices.Append({ .pos = pos5, .normal = normal, .uv = uvs[indices[1].uv - 1] });
                vertices.Append({ .pos = pos6, .normal = normal, .uv = uvs[indices[0].uv - 1] });
            }
            else {
                return false;
            }
        }
    }

    DynamicArray<ObjModel, LinearAllocator> models(allocator);
    uint64 prevInd = 0;
    for (uint64 i = 0; i < modelEndVertexInds.size; i++) {
        ObjModel* model = models.Append();
        model->vertices = vertices.ToArray().Slice(prevInd, modelEndVertexInds[i]);
        prevInd = modelEndVertexInds[i];
    }

    result->models = models.ToArray();
    return true;
}
