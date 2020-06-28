#include "load_obj.h"

#include <km_common/km_os.h>

bool LoadObj(const_string filePath, LoadObjResult* result, LinearAllocator* allocator)
{
    result->file = LoadEntireFile(filePath, allocator);
    if (result->file.data == nullptr) {
        return false;
    }

    DynamicArray<Vec3, LinearAllocator> vertexPositions(allocator);
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

            Vec3* v = vertexPositions.Append();
            int numElements;
            if (!StringToElementArray(next, ' ', false, StringToFloat32, 3, v->e, &numElements)) {
                return false;
            }
            if (numElements != 3) {
                return false;
            }
        }
        else if (next[0] == 'f' && next[1] == ' ') {
            next.data += 2;
            next.size -= 2;

            int inds[4];
            int numElements;
            if (!StringToElementArray(next, ' ', false, StringToIntBase10, 4, inds, &numElements)) {
                return false;
            }

            // NOTE obj files store faces in counter-clockwise order, but we want to return clockwise
            if (numElements == 3) {
                const Vec3 pos1 = vertexPositions[inds[0] - 1];
                const Vec3 pos2 = vertexPositions[inds[2] - 1];
                const Vec3 pos3 = vertexPositions[inds[1] - 1];
                const Vec3 normal = CalculateTriangleUnitNormal(pos1, pos2, pos3);

                vertices.Append({ .pos = pos1, .normal = normal });
                vertices.Append({ .pos = pos2, .normal = normal });
                vertices.Append({ .pos = pos3, .normal = normal });
            }
            else if (numElements == 4) {
                const Vec3 pos1 = vertexPositions[inds[0] - 1];
                const Vec3 pos2 = vertexPositions[inds[3] - 1];
                const Vec3 pos3 = vertexPositions[inds[2] - 1];
                const Vec3 pos4 = vertexPositions[inds[2] - 1];
                const Vec3 pos5 = vertexPositions[inds[1] - 1];
                const Vec3 pos6 = vertexPositions[inds[0] - 1];
                const Vec3 normal = CalculateTriangleUnitNormal(pos1, pos2, pos3);

                vertices.Append({ .pos = pos1, .normal = normal });
                vertices.Append({ .pos = pos2, .normal = normal });
                vertices.Append({ .pos = pos3, .normal = normal });
                vertices.Append({ .pos = pos4, .normal = normal });
                vertices.Append({ .pos = pos5, .normal = normal });
                vertices.Append({ .pos = pos6, .normal = normal });
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
