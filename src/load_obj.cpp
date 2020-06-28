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
    DynamicArray<int, LinearAllocator> startVertexInds(allocator);
    int startVertexInd = 0;

    string fileString = {
        .size = result->file.size,
        .data = (char*)result->file.data
    };
    string next;
    bool firstModel = true;
    while (true) {
        next = NextSplitElement(&fileString, '\n');
        if (next.size == 0) break;
        if (next.size < 2) continue;

        if (next[0] == 'o' && next[1] == ' ') {
            if (firstModel) {
                firstModel = false;
            }
            else {
                startVertexInds.Append(startVertexInd);
                startVertexInd = (int)vertices.size;
            }
        }
        if (next[0] == 'v' && next[1] == ' ') {
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
                Vertex* v1 = vertices.Append();
                Vertex* v2 = vertices.Append();
                Vertex* v3 = vertices.Append();
                v1->pos = vertexPositions[inds[0] - 1];
                v2->pos = vertexPositions[inds[2] - 1];
                v3->pos = vertexPositions[inds[1] - 1];

                const Vec3 normal = CalculateTriangleUnitNormal(v1->pos, v2->pos, v3->pos);
                v1->normal = normal;
                v2->normal = normal;
                v3->normal = normal;
            }
            else if (numElements == 4) {
                Vertex* v1 = vertices.Append();
                Vertex* v2 = vertices.Append();
                Vertex* v3 = vertices.Append();
                Vertex* v4 = vertices.Append();
                Vertex* v5 = vertices.Append();
                Vertex* v6 = vertices.Append();
                v1->pos = vertexPositions[inds[0] - 1];
                v2->pos = vertexPositions[inds[3] - 1];
                v3->pos = vertexPositions[inds[2] - 1];
                v4->pos = vertexPositions[inds[2] - 1];
                v5->pos = vertexPositions[inds[1] - 1];
                v6->pos = vertexPositions[inds[0] - 1];

                const Vec3 normal = CalculateTriangleUnitNormal(v1->pos, v2->pos, v3->pos);
                v1->normal = normal;
                v2->normal = normal;
                v3->normal = normal;
                v4->normal = normal;
                v5->normal = normal;
                v6->normal = normal;
            }
            else {
                return false;
            }
        }
    }

    DynamicArray<ObjModel, LinearAllocator> models(allocator);
    int prevInd = 0;
    for (uint64 i = 0; i < startVertexInds.size; i++) {
        ObjModel* model = models.Append();
        model->vertices = vertices.ToArray().Slice(prevInd, startVertexInds[i]);
        prevInd = startVertexInds[i];
    }

    result->models = models.ToArray();
    return true;
}
