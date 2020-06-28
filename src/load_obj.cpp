#include "load_obj.h"

template <typename Allocator>
bool LoadObj(const_string filePath, LoadObjResult* result, Allocator* allocator)
{
    result->file = LoadEntireFile(filePath, allocator);
    if (result->file.data == nullptr) {
        return false;
    }

    DynamicArray<ObjModel, Allocator> models(allocator);
    DynamicArray<Vec3, Allocator> vertices(allocator);
    DynamicArray<uint32_t, Allocator> indices(allocator);

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
                ObjModel* model = models.Append();
                model->vertices = vertices.ToArray();
                model->indices = indices.ToArray();
            }
        }
        if (next[0] == 'v' && next[1] == ' ') {
            next.data += 2;
            next.size -= 2;

            Vec3* v = vertices.Append();
            int numElements;
            if (!StringToElementArray(next, ' ', false, StringToFloat32, 3, v->e, &numElements)) {
                indices.Free();
                vertices.Free();
                FreeFile(result->file, allocator);
                return false;
            }
            if (numElements != 3) {
                indices.Free();
                vertices.Free();
                FreeFile(result->file, allocator);
                return false;
            }
        }
        else if (next[0] == 'f' && next[1] == ' ') {
            next.data += 2;
            next.size -= 2;

            int inds[4];
            int numElements;
            if (!StringToElementArray(next, ' ', false, StringToIntBase10, 4, inds, &numElements)) {
                indices.Free();
                vertices.Free();
                FreeFile(result->file, allocator);
                return false;
            }

            // NOTE obj files store faces in counter-clockwise order, but we want to return clockwise
            if (numElements == 3) {
                indices.Append(inds[0] - 1);
                indices.Append(inds[2] - 1);
                indices.Append(inds[1] - 1);
            }
            else if (numElements == 4) {
                indices.Append(inds[0] - 1);
                indices.Append(inds[3] - 1);
                indices.Append(inds[2] - 1);
                indices.Append(inds[2] - 1);
                indices.Append(inds[1] - 1);
                indices.Append(inds[0] - 1);
            }
            else {
                indices.Free();
                vertices.Free();
                FreeFile(result->file, allocator);
                return false;
            }
        }
    }

    result->models = models.ToArray();
    return true;
}

template <typename Allocator>
void FreeObj(const LoadObjResult& objResult, Allocator* allocator)
{
    for (uint64 i = 0; i < objResult.models.size; i++) {
        allocator->Free(objResult.models[i].vertices.data);
        allocator->Free(objResult.models[i].indices.data);
    }
    allocator->Free(objResult.models.data);

    FreeFile(objResult.file, allocator);
}
