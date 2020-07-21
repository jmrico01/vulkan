#include "main.h"

#include <intrin.h>
#include <stdio.h>

#include <stb_image.h>

#include <km_common/km_array.h>
#include <km_common/km_defines.h>
#include <km_common/km_load_obj.h>
#include <km_common/km_os.h>
#include <km_common/km_string.h>
#include <km_common/app/km_app.h>

#include "imgui.h"
#include "lightmap.h"

#define ENABLE_THREADS 1
#define ENABLE_LIGHTMAPPED_MESH 0

#define DEFINE_BLOCK_ROTATION_MATRICES const Mat4 top    = Translate(Vec3::unitZ); \
const Mat4 bottom = UnitQuatToMat4(QuatFromAngleUnitAxis(PI_F, Normalize(Vec3 { 1.0f, 1.0f, 0.0f }))); \
const Mat4 left   = Translate(Vec3 { 0.0f, 1.0f, 1.0f }) * \
UnitQuatToMat4(QuatFromAngleUnitAxis(-PI_F / 2.0f, Vec3::unitX)); \
const Mat4 right  = UnitQuatToMat4(QuatFromAngleUnitAxis(PI_F / 2.0f, Vec3::unitX)); \
const Mat4 front  = Translate(Vec3 { 1.0f, 0.0f, 1.0f }) * \
UnitQuatToMat4(QuatFromAngleUnitAxis(PI_F / 2.0f, Vec3::unitY)); \
const Mat4 back   = UnitQuatToMat4(QuatFromAngleUnitAxis(-PI_F / 2.0f, Vec3::unitY));

/*
TODO

> post-process pipeline for grain

*/

// Required for platform main
const char* WINDOW_NAME = "vulkan";
const int WINDOW_START_WIDTH  = 1600;
const int WINDOW_START_HEIGHT = 900;
const bool WINDOW_LOCK_CURSOR = true;
const uint64 PERMANENT_MEMORY_SIZE = MEGABYTES(1);
const uint64 TRANSIENT_MEMORY_SIZE = MEGABYTES(512);

const float32 DEFAULT_BLOCK_SIZE = 1.0f;
const uint32 DEFAULT_STREET_SIZE = 3;
const uint32 DEFAULT_SIDEWALK_SIZE = 2;
const uint32 DEFAULT_BUILDING_SIZE = 10;
const uint32 DEFAULT_BUILDING_HEIGHT = 3;

const float32 BLOCK_COLLISION_MARGIN = 0.2f;

const float32 DEFAULT_MOB_SPAWN_FREQ = 0.01f;

internal AppState* GetAppState(AppMemory* memory)
{
    DEBUG_ASSERT(sizeof(AppState) < memory->permanent.size);
    return (AppState*)memory->permanent.data;
}

internal TransientState* GetTransientState(AppMemory* memory)
{
    DEBUG_ASSERT(sizeof(TransientState) < memory->transient.size);
    TransientState* transientState = (TransientState*)memory->transient.data;
    transientState->scratch = {
        .size = memory->transient.size - sizeof(TransientState),
        .data = memory->transient.data + sizeof(TransientState),
    };
    return transientState;
}

Vec3Int WorldPosToBlockIndex(Vec3 worldPos, float32 blockSize, Vec3Int blocksSize, Vec3Int blockOrigin)
{
    const int x = (int)(FloorFloat32(worldPos.x / blockSize)) + blockOrigin.x;
    const int y = (int)(FloorFloat32(worldPos.y / blockSize)) + blockOrigin.y;
    const int z = (int)(FloorFloat32(worldPos.z / blockSize)) + blockOrigin.z;
    if (0 <= x && x < blocksSize.x && 0 <= y && y < blocksSize.y && 0 <= z && z < blocksSize.z) {
        return Vec3Int { x, y, z };
    }

    return Vec3Int { -1, -1, -1 };
}

Vec3 BlockIndexToWorldPos(Vec3Int blockIndex, float32 blockSize, Vec3Int blockOrigin)
{
    return Vec3 {
        (float32)(blockIndex.x - blockOrigin.x) * blockSize,
        (float32)(blockIndex.y - blockOrigin.y) * blockSize,
        (float32)(blockIndex.z - blockOrigin.z) * blockSize,
    };
}

bool IsWalkable(const BlockGrid& blockGrid, Vec3Int blockIndex)
{
    if (blockIndex.x < 0 || blockIndex.y < 0 || blockIndex.z < 0) {
        return false;
    }

    const Block& block = blockGrid[blockIndex.z][blockIndex.y][blockIndex.x];
    return block.id == BlockId::NONE;
}

void GenerateCityBlocks(uint32 streetSize, uint32 sidewalkSize, uint32 buildingSize, uint32 buildingHeight,
                        LinearAllocator* allocator, BlockGrid* blockGrid)
{
    MemSet(blockGrid, 0, sizeof(BlockGrid));

    const uint32 cityBlockSize = streetSize + sidewalkSize * 2 + buildingSize;

    Array<uint8> streetMask = allocator->NewArray<uint8>(cityBlockSize);
    for (uint32 i = 0; i < cityBlockSize; i++) {
        streetMask[i] = i < streetSize;
    }
    Array<uint8> sidewalkMask = allocator->NewArray<uint8>(cityBlockSize);
    for (uint32 i = 0; i < cityBlockSize; i++) {
        sidewalkMask[i] = (i >= streetSize && i < (streetSize + sidewalkSize))
            || (i >= (streetSize + sidewalkSize + buildingSize) && i < (streetSize + 2 * sidewalkSize + buildingSize));
    }
    Array<uint8> buildingMask = allocator->NewArray<uint8>(cityBlockSize);
    for (uint32 i = 0; i < cityBlockSize; i++) {
        buildingMask[i] = i >= (streetSize + sidewalkSize) && i < (streetSize + sidewalkSize + buildingSize);
    }

    static_assert(BLOCK_ORIGIN.z > 0);
    for (uint32 z = 0; z < blockGrid->SIZE; z++) {
        for (uint32 y = 0; y < (*blockGrid)[z].SIZE; y++) {
            for (uint32 x = 0; x < (*blockGrid)[z][y].SIZE; x++) {
                const uint32 cityBlockX = x % cityBlockSize;
                const uint32 cityBlockY = y % cityBlockSize;

                const uint8 building = buildingMask[cityBlockX] * buildingMask[cityBlockY];
                if ((int)z == BLOCK_ORIGIN.z - 1) {
                    const uint8 street = streetMask[cityBlockX] + streetMask[cityBlockY];
                    const uint8 sidewalk = sidewalkMask[cityBlockX] + sidewalkMask[cityBlockY];
                    if (street) {
                        (*blockGrid)[z][y][x].id = BlockId::STREET;
                    }
                    else if (sidewalk) {
                        (*blockGrid)[z][y][x].id = BlockId::SIDEWALK;
                    }
                    else if (building) {
                        (*blockGrid)[z][y][x].id = BlockId::BUILDING;
                    }
                }
                else if (building && z - BLOCK_ORIGIN.z < buildingHeight) {
                    (*blockGrid)[z][y][x].id = BlockId::BUILDING;
                }
            }
        }
    }
}

void SpawnMobs(const BlockGrid& blockGrid, float32 blockSize, Vec3Int blockOrigin, float32 spawnFreq,
               FixedArray<Mob, AppState::MAX_MOBS>* mobs)
{
    const Vec3 hitboxRadius = { 0.5f, 0.5f, 1.3f };

    mobs->Clear();
    for (uint32 z = 0; z < blockGrid.SIZE; z++) {
        for (uint32 y = 0; y < blockGrid[z].SIZE; y++) {
            for (uint32 x = 0; x < blockGrid[z][y].SIZE; x++) {
                const Block& block = blockGrid[z][y][x];
                if (mobs->size < mobs->MAX_SIZE && (int)z == blockOrigin.z
                    && block.id == BlockId::NONE && RandFloat32() < spawnFreq) {
                    const Vec3 blockPos = BlockIndexToWorldPos(Vec3Int { (int)x, (int)y, (int)z }, blockSize, blockOrigin);

                    Mob* mob = mobs->Append();
                    mob->pos = blockPos + Vec3 { blockSize / 2.0f, blockSize / 2.0f, 2.6f } ;
                    mob->yaw = RandFloat32() * 2.0f * PI_F;
                    mob->hitbox = {
                        .min = mob->pos - hitboxRadius,
                        .max = mob->pos + hitboxRadius
                    };
                    mob->collapseT = 0.0f;
                    mob->collapsed = false;
                }
            }
        }
    }
}

bool LoadBlockGrid(const_string filePath, BlockGrid* blockGrid, LinearAllocator* allocator)
{
    Array<uint8> data = LoadEntireFile(filePath, allocator);
    if (data.data == nullptr) {
        return false;
    }
    if (data.size != sizeof(Vec3Int) + sizeof(BlockGrid)) {
        return false;
    }

    const Vec3Int* fileBlockGridSize = (Vec3Int*)data.data;
    const BlockGrid* fileBlockGrid = (BlockGrid*)(data.data + sizeof(Vec3Int));
    if ((uint32)fileBlockGridSize->x != (*blockGrid)[0][0].SIZE
        || (uint32)fileBlockGridSize->y != (*blockGrid)[0].SIZE
        || (uint32)fileBlockGridSize->z != blockGrid->SIZE) {
        return false;
    }

    MemCopy(blockGrid, fileBlockGrid, sizeof(BlockGrid));

    return true;
}

bool SaveBlockGrid(const_string filePath, const BlockGrid& blockGrid, LinearAllocator* allocator)
{
    const Vec3Int blockGridSize = { (int)blockGrid[0][0].SIZE, (int)blockGrid[0].SIZE, (int)blockGrid.SIZE };

    Array<uint8> data = allocator->NewArray<uint8>(sizeof(Vec3Int) + sizeof(BlockGrid));
    MemCopy(data.data, &blockGridSize, sizeof(Vec3Int));
    MemCopy(data.data + sizeof(Vec3Int), &blockGrid, sizeof(BlockGrid));

    return WriteFile(filePath, data, false);
}

APP_UPDATE_AND_RENDER_FUNCTION(AppUpdateAndRender)
{
    UNREFERENCED_PARAMETER(queue);
    UNREFERENCED_PARAMETER(audio);

    AppState* appState = GetAppState(memory);
    TransientState* transientState = GetTransientState(memory);

    const Vec2Int screenSize = {
        (int)vulkanState.swapchain.extent.width,
        (int)vulkanState.swapchain.extent.height
    };

    const float32 CAMERA_HEIGHT = 1.7f;

    // Initialize memory if necessary
    if (!memory->initialized) {
        {
            LinearAllocator allocator(transientState->scratch);

#if 1
            const_string saveFilePath = ToString("data/0.blockgrid");
            if (LoadBlockGrid(saveFilePath, &appState->blockGrid, &allocator)) {
                LOG_INFO("Loaded block grid from %.*s\n", saveFilePath.size, saveFilePath.data);
            }
            else {
                LOG_ERROR("Failed to load block grid from %.*s\n", saveFilePath.size, saveFilePath.data);
            }

#if 0
            for (uint32 z = 0; z < appState->blockGrid.SIZE; z++) {
                for (uint32 y = 0; y < appState->blockGrid[z].SIZE; y++) {
                    for (uint32 x = 0; x < appState->blockGrid[z][y].SIZE; x++) {
                        appState->blockGrid[z][y][x].id = BlockId::SIDEWALK;
                    }
                }
            }
            appState->blockGrid[BLOCK_ORIGIN.z][BLOCK_ORIGIN.y][BLOCK_ORIGIN.x].id = BlockId::NONE;
            appState->blockGrid[BLOCK_ORIGIN.z + 1][BLOCK_ORIGIN.y][BLOCK_ORIGIN.x].id = BlockId::NONE;
#endif

#else
            GenerateCityBlocks(DEFAULT_STREET_SIZE, DEFAULT_SIDEWALK_SIZE,
                               DEFAULT_BUILDING_SIZE, DEFAULT_BUILDING_HEIGHT, &allocator, &appState->blockGrid);
#endif
        }
        appState->blockSize = DEFAULT_BLOCK_SIZE;

        const Vec3 startPos = Vec3 { 0.5f, 0.5f, CAMERA_HEIGHT };

        appState->cameraPos = startPos;
        appState->cameraAngles = Vec2 { 0.0f, 0.0f };

        // SpawnMobs(appState->blockGrid, appState->blockSize, BLOCK_ORIGIN, DEFAULT_MOB_SPAWN_FREQ, &appState->mobs);

        appState->collapsingMobIndex = appState->mobs.size;

        // Debug views 
        appState->debugView = false;

        appState->noclip = false;
        appState->noclipPos = startPos;

        appState->cityGenMinimized = true;
        appState->sliderBlockSize.value = appState->blockSize;
        appState->sliderMobSpawnFreq.value = DEFAULT_MOB_SPAWN_FREQ;
        appState->inputStreetSize.Initialize(DEFAULT_STREET_SIZE);
        appState->inputSidewalkSize.Initialize(DEFAULT_SIDEWALK_SIZE);
        appState->inputBuildingSize.Initialize(DEFAULT_BUILDING_SIZE);
        appState->inputBuildingHeight.Initialize(DEFAULT_BUILDING_HEIGHT);

        appState->blockEditor = false;

        memory->initialized = true;
    }

    // Reset frame state
    {
        ResetSpriteRenderState(&transientState->frameState.spriteRenderState);
        ResetTextRenderState(&transientState->frameState.textRenderState);

        ResetMeshRenderState(&transientState->frameState.meshRenderState);
    }

    appState->elapsedTime += deltaTime;

    if (KeyPressed(input, KM_KEY_G)) {
        appState->debugView = !appState->debugView;
        if (!appState->debugView) {
            LockCursor(true);
        }
        appState->blockEditor = false;
    }

    if (KeyPressed(input, KM_KEY_ESCAPE)) {
        bool cursorLocked = IsCursorLocked();
        LockCursor(!cursorLocked);
    }

    if (KeyPressed(input, KM_KEY_N)) {
        appState->noclip = !appState->noclip;
    }

#if ENABLE_LIGHTMAPPED_MESH
    if (KeyPressed(input, KM_KEY_L)) {
        LinearAllocator allocator(transientState->scratch);

        LoadObjResult obj;
        if (LoadObj(ToString("data/models/reference-scene-small.obj"), &obj, &allocator)) {
            if (GenerateLightmaps(obj, LIGHTMAP_NUM_BOUNCES, queue, &allocator, ToString("data/lightmaps"))) {
                AppUnloadVulkanSwapchainState(vulkanState, memory);
                AppUnloadVulkanWindowState(vulkanState, memory);
                if (!AppLoadVulkanWindowState(vulkanState, memory)) {
                    LOG_ERROR("Failed to reload Vulkan state after lightmap generation\n");
                }
                if (!AppLoadVulkanSwapchainState(vulkanState, memory)) {
                    LOG_ERROR("Failed to reload Vulkan state after lightmap generation\n");
                }
            }
            else {
                LOG_ERROR("Failed to generate lightmaps\n");
            }
        }
        else {
            LOG_ERROR("Failed to load scene .obj when generating lightmaps\n");
        }
    }
#endif

    const float32 cameraSensitivity = 2.0f;
    const Vec2 mouseDeltaFrac = {
        (float32)input.mouseDelta.x / (float32)screenSize.x,
        (float32)input.mouseDelta.y / (float32)screenSize.y
    };
    if (IsCursorLocked() || MouseDown(input, KM_MOUSE_RIGHT)) {
        appState->cameraAngles.x += mouseDeltaFrac.x * cameraSensitivity;
        appState->cameraAngles.y -= mouseDeltaFrac.y * cameraSensitivity;
    }

    appState->cameraAngles.x = ModFloat32(appState->cameraAngles.x, PI_F * 2.0f);
    appState->cameraAngles.y = ClampFloat32(appState->cameraAngles.y, -PI_F / 2.0f, PI_F / 2.0f);

    const Quat cameraRotYaw = QuatFromAngleUnitAxis(appState->cameraAngles.x, Vec3::unitZ);
    const Quat cameraRotPitch = QuatFromAngleUnitAxis(appState->cameraAngles.y, Vec3::unitY);

    const Quat cameraRotYawInv = Inverse(cameraRotYaw);
    const Vec3 cameraForwardXY = cameraRotYawInv * Vec3::unitX;
    const Vec3 cameraRightXY = cameraRotYawInv * -Vec3::unitY;
    const Vec3 cameraUp = Vec3::unitZ;

    float32 speed = 5.0f;
    if (KeyDown(input, KM_KEY_SHIFT)) {
        if (appState->noclip) {
            speed *= 3.0f;
        }
        else {
            speed *= 1.6f;
        }
    }

    Vec3 velocity = Vec3::zero;
    if (KeyDown(input, KM_KEY_W)) {
        velocity += cameraForwardXY;
    }
    if (KeyDown(input, KM_KEY_S)) {
        velocity -= cameraForwardXY;
    }
    if (KeyDown(input, KM_KEY_A)) {
        velocity -= cameraRightXY;
    }
    if (KeyDown(input, KM_KEY_D)) {
        velocity += cameraRightXY;
    }
    if (appState->noclip) {
        if (KeyDown(input, KM_KEY_SPACE)) {
            velocity += cameraUp;
        }
        if (KeyDown(input, KM_KEY_CTRL)) {
            velocity -= cameraUp;
        }
    }

    if (velocity != Vec3::zero) {
        velocity = Normalize(velocity) * speed * deltaTime;
        if (appState->noclip) {
            appState->noclipPos += velocity;
        }
        else {
            const Vec3 prevPos = appState->noclip ? appState->noclipPos : appState->cameraPos;
            const Vec3Int prevBlockIndex = WorldPosToBlockIndex(prevPos, appState->blockSize, BLOCKS_SIZE, BLOCK_ORIGIN);

            Rect range = {
                .min = Vec2 { -1e8, -1e8 },
                .max = Vec2 {  1e8,  1e8 },
            };
            if (prevBlockIndex.x >= 0 && prevBlockIndex.y >= 0) {
                const Vec3 blockOrigin = BlockIndexToWorldPos(prevBlockIndex, appState->blockSize, BLOCK_ORIGIN);
                if (!IsWalkable(appState->blockGrid, prevBlockIndex - Vec3Int::unitX)) {
                    range.min.x = blockOrigin.x + BLOCK_COLLISION_MARGIN;
                }
                if (!IsWalkable(appState->blockGrid, prevBlockIndex + Vec3Int::unitX)) {
                    range.max.x = blockOrigin.x + appState->blockSize - BLOCK_COLLISION_MARGIN;
                }
                if (!IsWalkable(appState->blockGrid, prevBlockIndex - Vec3Int::unitY)) {
                    range.min.y = blockOrigin.y + BLOCK_COLLISION_MARGIN;
                }
                if (!IsWalkable(appState->blockGrid, prevBlockIndex + Vec3Int::unitY)) {
                    range.max.y = blockOrigin.y + appState->blockSize - BLOCK_COLLISION_MARGIN;
                }
            }

            Vec3 newPos = appState->cameraPos + velocity;
            const Vec2 prevPos2 = { prevPos.x, prevPos.y };
            if (IsWalkable(appState->blockGrid, prevBlockIndex)) {
                newPos.x = ClampFloat32(newPos.x, range.min.x, range.max.x);
                newPos.y = ClampFloat32(newPos.y, range.min.y, range.max.y);
            }
            appState->cameraPos = newPos;
        }
    }

    const Vec3 currentPos = appState->noclip ? appState->noclipPos : appState->cameraPos;
    const Vec3Int blockIndex = WorldPosToBlockIndex(currentPos, appState->blockSize, BLOCKS_SIZE, BLOCK_ORIGIN);

    const Quat cameraRot = cameraRotPitch * cameraRotYaw;
    const Mat4 cameraRotMat4 = UnitQuatToMat4(cameraRot);

    const Quat inverseCameraRot = Inverse(cameraRot);
    const Vec3 cameraForward = inverseCameraRot * Vec3::unitX;

    uint32 hoverMobIndex = appState->mobs.size;
    float32 hoverMobMinDist = 1e8;
    for (uint32 i = 0; i < appState->mobs.size; i++) {
        const Mob& mob = appState->mobs[i];
        if (mob.collapsed) continue;

        const Vec3 rayOrigin = appState->noclip ? appState->noclipPos : appState->cameraPos;
        const Vec3 inverseRayDir = Reciprocal(cameraForward);
        // const float32 hitboxCollapsedScale = MinFloat32(1.0f, 1.2f - mob.collapseT);
        const float32 hitboxCollapsedScale = 1.0f;
        const Box hitboxCollapsed = {
            .min = mob.hitbox.min * hitboxCollapsedScale,
            .max = mob.hitbox.max * hitboxCollapsedScale
        };
        float32 t;
        if (RayAxisAlignedBoxIntersection(rayOrigin, inverseRayDir, hitboxCollapsed, &t)) {
            if (t < hoverMobMinDist) {
                hoverMobIndex = i;
                hoverMobMinDist = t;
            }
        }
    }

    if (appState->collapsingMobIndex != appState->mobs.size) {
        if (appState->collapsingMobIndex != hoverMobIndex || !MouseDown(input, KM_MOUSE_LEFT)) {
            appState->collapsingMobIndex = appState->mobs.size;
        }
    }
    else if (hoverMobIndex != appState->mobs.size && MousePressed(input, KM_MOUSE_LEFT)) {
        appState->collapsingMobIndex = hoverMobIndex;
    }

    const float32 totalCollapseTime = 2.0f;
    const float32 totalUncollapseTime = 0.6f;

    for (uint32 i = 0; i < appState->mobs.size; i++) {
        Mob& mob = appState->mobs[i];

        if (i == appState->collapsingMobIndex) {
            mob.collapseT += deltaTime / totalCollapseTime;
            if (mob.collapseT >= 1.0f) {
                mob.collapsed = true;
                appState->collapsingMobIndex = appState->mobs.size;
            }
        }
        else if (mob.collapseT < 1.0f) {
            mob.collapseT -= deltaTime / totalUncollapseTime;
        }

        mob.collapseT = MaxFloat32(mob.collapseT, 0.0f);
    }

    // Transforms world-view camera (+X forward, +Z up) to Vulkan camera (+Z forward, -Y up)
    const Quat baseCameraRot = QuatFromAngleUnitAxis(-PI_F / 2.0f, Vec3::unitY)
        * QuatFromAngleUnitAxis(PI_F / 2.0f, Vec3::unitX);
    const Mat4 baseCameraRotMat4 = UnitQuatToMat4(baseCameraRot);

    const Mat4 view = baseCameraRotMat4 * cameraRotMat4 * Translate(-currentPos);

    const float32 aspect = (float32)screenSize.x / (float32)screenSize.y;
    const float32 nearZ = 0.1f;
    const float32 farZ = 100.0f;
    const Mat4 proj = Perspective(PI_F / 4.0f, aspect, nearZ, farZ);

    // Debug views
    if (appState->debugView) {
        LinearAllocator allocator(transientState->scratch);

        const VulkanFontFace& fontNormal = appState->fontFaces[(uint32)FontId::OCR_A_REGULAR_18];
        const VulkanFontFace& fontTitle = appState->fontFaces[(uint32)FontId::OCR_A_REGULAR_24];

        const Vec4 backgroundColor = Vec4 { 0.0f, 0.0f, 0.0f, 0.5f };
        const Vec4 inputTextColor = { 1.0f, 1.0f, 1.0f, 1.0f };
        const Vec2Int panelBorderSize = Vec2Int { 6, 8 };
        const int panelPosMargin = 50;

        // General debug info
        const Vec2Int panelDebugInfoPos = { screenSize.x - panelPosMargin, panelPosMargin };
        Panel panelDebugInfo(&allocator);
        panelDebugInfo.Begin(input, &fontNormal, PanelFlag::GROW_DOWNWARDS, panelDebugInfoPos, 1.0f);

        const int fps = (int)(1.0f / deltaTime);
        const float32 frameMs = deltaTime * 1000.0f;
        const_string frameTiming = AllocPrintf(&allocator, "%d fps | %.03f ms", fps, frameMs);
        panelDebugInfo.Text(frameTiming);
        panelDebugInfo.Text(string::empty);

        const_string cameraPosString = AllocPrintf(&allocator, "%.02f, %.02f, %.02f",
                                                   appState->cameraPos.x, appState->cameraPos.y, appState->cameraPos.z);
        const_string noclipPosString = AllocPrintf(&allocator, "%.02f, %.02f, %.02f",
                                                   appState->noclipPos.x, appState->noclipPos.y, appState->noclipPos.z);
        if (appState->noclip) {
            panelDebugInfo.Text(noclipPosString);
        }
        else {
            panelDebugInfo.Text(cameraPosString);
        }

        const_string blockIndexString = AllocPrintf(&allocator, "%d, %d, %d",
                                                    blockIndex.x, blockIndex.y, blockIndex.z);
        panelDebugInfo.Text(blockIndexString);

        panelDebugInfo.Text(string::empty);

        bool cursorLocked = IsCursorLocked();
        if (panelDebugInfo.Checkbox(&cursorLocked, ToString("cam lock (ESC)"))) {
            LockCursor(cursorLocked);
        }
        panelDebugInfo.Checkbox(&appState->noclip, ToString("noclip (N)"));

        panelDebugInfo.Draw(panelBorderSize, Vec4::one, backgroundColor, screenSize,
                            &transientState->frameState.spriteRenderState, &transientState->frameState.textRenderState);

        // City block generator
        const Vec2Int panelCityGenPos = { panelPosMargin, panelPosMargin };
        Panel panelCityGen(&allocator);
        panelCityGen.Begin(input, &fontNormal, PanelFlag::GROW_DOWNWARDS, panelCityGenPos, 0.0f);

        panelCityGen.TitleBar(ToString("City Block Generator"), &appState->cityGenMinimized, Vec4::zero, &fontTitle);

        panelCityGen.Text(ToString("change parameters and"));
        panelCityGen.Text(ToString("press \"Generate\""));
        panelCityGen.Text(string::empty);

        panelCityGen.Text(AllocPrintf(&allocator, "%d x %d x %d blocks",
                                      BLOCKS_SIZE.x, BLOCKS_SIZE.y, BLOCKS_SIZE.z));

        panelCityGen.Text(ToString("block size:"));
        if (panelCityGen.SliderFloat(&appState->sliderBlockSize, 1.0f, 4.0f)) {
            appState->blockSize = appState->sliderBlockSize.value;
        }

        panelCityGen.Text(ToString("mob spawn frequency:"));
        panelCityGen.SliderFloat(&appState->sliderMobSpawnFreq, 0.0f, 1.0f);

        panelCityGen.Text(ToString("street size:"));
        panelCityGen.InputInt(&appState->inputStreetSize, inputTextColor);

        panelCityGen.Text(ToString("sidewalk size:"));
        panelCityGen.InputInt(&appState->inputSidewalkSize, inputTextColor);

        panelCityGen.Text(ToString("building size:"));
        panelCityGen.InputInt(&appState->inputBuildingSize, inputTextColor);

        panelCityGen.Text(ToString("building height:"));
        panelCityGen.InputInt(&appState->inputBuildingHeight, inputTextColor);

        if (panelCityGen.Button(ToString("Generate"))) {
            if (appState->inputStreetSize.valid && appState->inputSidewalkSize.valid
                && appState->inputBuildingSize.valid && appState->inputBuildingHeight.valid) {
                LOG_INFO("Re-generating city blocks...\n");
                GenerateCityBlocks(appState->inputStreetSize.value, appState->inputSidewalkSize.value,
                                   appState->inputBuildingSize.value, appState->inputBuildingHeight.value,
                                   &allocator, &appState->blockGrid);

                SpawnMobs(appState->blockGrid, appState->blockSize, BLOCK_ORIGIN, appState->sliderMobSpawnFreq.value,
                          &appState->mobs);
            }
        }

        panelCityGen.Draw(panelBorderSize, Vec4::one, backgroundColor, screenSize,
                          &transientState->frameState.spriteRenderState, &transientState->frameState.textRenderState);

        // Block editor
        const float32 BLOCK_INTERACT_DIST = appState->blockSize * 8.0f;
        const Vec2Int panelBlockEditorPos = { panelPosMargin, screenSize.y - panelPosMargin };

        Vec3Int hitIndex = { -1, -1, -1 };
        float32 hitMinDist = 1e8;
        {
            const Vec3 rayOrigin = appState->noclip ? appState->noclipPos : appState->cameraPos;
            const Vec3 inverseRayDir = Reciprocal(cameraForward);

            for (uint32 z = 0; z < appState->blockGrid.SIZE; z++) {
                for (uint32 y = 0; y < appState->blockGrid[z].SIZE; y++) {
                    for (uint32 x = 0; x < appState->blockGrid[z][y].SIZE; x++) {
                        if (appState->blockGrid[z][y][x].id == BlockId::NONE) {
                            continue;
                        }

                        const Vec3Int blockInd = { (int)x, (int)y, (int)z };
                        const Vec3 blockPos = BlockIndexToWorldPos(blockInd, appState->blockSize, BLOCK_ORIGIN);
                        const Box box = {
                            .min = blockPos,
                            .max = blockPos + Vec3 { appState->blockSize, appState->blockSize, appState->blockSize },
                        };
                        float32 t;
                        if (RayAxisAlignedBoxIntersection(rayOrigin, inverseRayDir, box, &t)) {
                            if (t > 0.0f && t < hitMinDist) {
                                hitIndex = blockInd;
                                hitMinDist = t;
                            }
                        }
                    }
                }
            }
        }

        Panel panelBlockEditor(&allocator);
        panelBlockEditor.Begin(input, &fontNormal, !PanelFlag::GROW_DOWNWARDS, panelBlockEditorPos, 0.0f);

        bool blockEditorMinimized = !appState->blockEditor;
        const bool blockEditorChanged = panelBlockEditor.TitleBar(ToString("Block Editor (B)"), &blockEditorMinimized,
                                                                  Vec4::zero, &fontTitle);
        if (blockEditorChanged || KeyPressed(input, KM_KEY_B)) {
            appState->blockEditor = !appState->blockEditor;
            if (appState->blockEditor) {
                LockCursor(true);
            }
        }

        panelBlockEditor.Text(string::empty);

        if (panelBlockEditor.Button(ToString("Clear mobs (C)")) || KeyPressed(input, KM_KEY_C)) {
            appState->mobs.Clear();
        }

        panelBlockEditor.Text(string::empty);

        const_string saveFilePath = ToString("data/0.blockgrid");
        if (panelBlockEditor.Button(ToString("Save"))) {
            if (SaveBlockGrid(saveFilePath, appState->blockGrid, &allocator)) {
                LOG_INFO("Saved block grid to %.*s\n", saveFilePath.size, saveFilePath.data);
            }
            else {
                LOG_ERROR("Failed to save block grid to %.*s\n", saveFilePath.size, saveFilePath.data);
            }
        }
        if (panelBlockEditor.Button(ToString("Load"))) {
            if (LoadBlockGrid(saveFilePath, &appState->blockGrid, &allocator)) {
                LOG_INFO("Loaded block grid from %.*s\n", saveFilePath.size, saveFilePath.data);
            }
            else {
                LOG_ERROR("Failed to load block grid from %.*s\n", saveFilePath.size, saveFilePath.data);
            }
        }

        if (hitIndex.x != -1) {
            panelBlockEditor.Text(AllocPrintf(&allocator, "block %d, %d, %d", hitIndex.x, hitIndex.y, hitIndex.z));
            panelBlockEditor.Text(AllocPrintf(&allocator, "distance %.02f", hitMinDist));

            if (appState->blockEditor && !blockEditorChanged && hitMinDist < BLOCK_INTERACT_DIST) {
                if (MousePressed(input, KM_MOUSE_LEFT)) {
                    appState->blockGrid[hitIndex.z][hitIndex.y][hitIndex.x].id = BlockId::NONE;
                }
                if (MousePressed(input, KM_MOUSE_RIGHT)) {
                    if (hitIndex.z != BLOCKS_SIZE.z - 1) {
                        if (KeyDown(input, KM_KEY_SHIFT)) {
                            const Vec3 hitboxRadius = { 0.5f, 0.5f, 1.3f };
                            const Vec3 blockPos = BlockIndexToWorldPos(hitIndex, appState->blockSize, BLOCK_ORIGIN);
                            const Vec3 mobOffset = Vec3 {
                                appState->blockSize / 2.0f,
                                appState->blockSize / 2.0f,
                                2.2f
                            };

                            Mob* newMob = appState->mobs.Append();
                            newMob->pos = blockPos + mobOffset;
                            newMob->yaw = RandFloat32() * 2.0f * PI_F;
                            newMob->hitbox = {
                                .min = newMob->pos - hitboxRadius,
                                .max = newMob->pos + hitboxRadius,
                            };
                            newMob->collapseT = 0.0f;
                            newMob->collapsed = false;
                        }
                        else {
                            appState->blockGrid[hitIndex.z + 1][hitIndex.y][hitIndex.x].id = BlockId::SIDEWALK;
                        }
                    }
                }
            }
        }

        panelBlockEditor.Draw(panelBorderSize, Vec4::one, backgroundColor, screenSize,
                              &transientState->frameState.spriteRenderState, &transientState->frameState.textRenderState);
    }

    // Draw mobs
    {
        for (uint32 i = 0; i < appState->mobs.size; i++) {
            const Mob& mob = appState->mobs[i];
            if (mob.collapsed) continue;

            const Mat4 model = Translate(mob.pos) * Rotate(Vec3 { 0.0f, 0.0f, mob.yaw }) * Scale(0.45f);
            PushMesh(MeshId::MOB, model, Vec3::one * 0.6f, Vec3::zero, mob.collapseT,
                     &transientState->frameState.meshRenderState);
        }
    }

    // Draw blocks
    {
        const Mat4 scale = Scale(appState->blockSize);

        DEFINE_BLOCK_ROTATION_MATRICES;

        for (uint32 z = 0; z < appState->blockGrid.SIZE; z++) {
            for (uint32 y = 0; y < appState->blockGrid[z].SIZE; y++) {
                for (uint32 x = 0; x < appState->blockGrid[z][y].SIZE; x++) {
                    const Block block = appState->blockGrid[z][y][x];

                    bool drawTop    = (int)z == BLOCKS_SIZE.z - 1 || appState->blockGrid[z + 1][y][x].id == BlockId::NONE;
                    bool drawLeft   = (int)y == BLOCKS_SIZE.y - 1 || appState->blockGrid[z][y + 1][x].id == BlockId::NONE;
                    bool drawFront  = (int)x == BLOCKS_SIZE.x - 1 || appState->blockGrid[z][y][x + 1].id == BlockId::NONE;
                    bool drawBottom = z != 0 && appState->blockGrid[z - 1][y][x].id == BlockId::NONE;
                    bool drawRight  = y == 0 || appState->blockGrid[z][y - 1][x].id == BlockId::NONE;
                    bool drawBack   = x == 0 || appState->blockGrid[z][y][x - 1].id == BlockId::NONE;

                    Vec3 color = Vec3::zero;
                    switch (block.id) {
                        case BlockId::NONE: {
                            drawTop = false;
                            drawLeft = false;
                            drawFront = false;
                            drawBottom = false;
                            drawRight = false;
                            drawBack = false;
                        } break;
                        case BlockId::SIDEWALK: {
                            color = Vec3::one * 0.7f;
                        } break;
                        case BlockId::STREET: {
                            color = Vec3::one * 0.5f;
                        } break;
                        case BlockId::BUILDING: {
                            color = Vec3::one * 0.8f;
                        } break;
                    }

#if 0
                    if (appState->blockEditor) {
                        if (z == appState->selectedZ) {
                            const Vec3 highlightColor = Vec3 { 1.0f, 0.0f, 0.0f };
                            const Vec3 selectedColor = Vec3 { 0.0f, 1.0f, 1.0f };
                            const float32 highlightPeriod = 1.0f;
                            const float32 highlightStrength = 0.1f;

                            const float32 t = Sin32(appState->elapsedTime * 2.0f * PI_F / highlightPeriod) / 2.0f + 0.5f;
                            color = Lerp(color, highlightColor, t * highlightStrength);

                            const bool hovered = hoveredX > 0 && hoveredY > 0 &&
                                x == (uint32)hoveredX && y == (uint32)hoveredY;
                            if (hovered) {
                                color = Lerp(color, selectedColor, t * highlightStrength * 2.0f);
                            }

                            if (block.id != BlockId::NONE || hovered) {
                                drawTop = true;
                                drawLeft = true;
                                drawFront = true;
                                drawBottom = true;
                                drawRight = true;
                                drawBack = true;
                            }
                        }
                        else {
                            drawTop = false;
                            drawLeft = false;
                            drawFront = false;
                            drawBottom = false;
                            drawRight = false;
                            drawBack = false;
                        }
                    }
#endif

                    const Vec3 pos = BlockIndexToWorldPos(Vec3Int { (int)x, (int)y, (int)z },
                                                          appState->blockSize, BLOCK_ORIGIN);
                    const Mat4 posTransform = Translate(pos);
                    if (drawTop) {
                        PushMesh(MeshId::TILE, posTransform * scale * top, color, Vec3::zero, 0.0f,
                                 &transientState->frameState.meshRenderState);
                    }
                    if (drawBottom) {
                        PushMesh(MeshId::TILE, posTransform * scale * bottom, color, Vec3::zero, 0.0f,
                                 &transientState->frameState.meshRenderState);
                    }
                    if (drawLeft) {
                        PushMesh(MeshId::TILE, posTransform * scale * left, color, Vec3::zero, 0.0f,
                                 &transientState->frameState.meshRenderState);
                    }
                    if (drawRight) {
                        PushMesh(MeshId::TILE, posTransform * scale * right, color, Vec3::zero, 0.0f,
                                 &transientState->frameState.meshRenderState);
                    }
                    if (drawFront) {
                        PushMesh(MeshId::TILE, posTransform * scale * front, color, Vec3::zero, 0.0f,
                                 &transientState->frameState.meshRenderState);
                    }
                    if (drawBack) {
                        PushMesh(MeshId::TILE, posTransform * scale * back, color, Vec3::zero, 0.0f,
                                 &transientState->frameState.meshRenderState);
                    }
                }
            }
        }
    }

    // Draw crosshair
    {
        const int crosshairPixels = 3;
        const int crosshairHalfPixels = crosshairPixels / 2;
        const Vec2Int crosshairPos = {
            screenSize.x / 2 - crosshairHalfPixels,
            screenSize.y / 2 - crosshairHalfPixels
        };
        const Vec2Int crosshairSize = { crosshairPixels, crosshairPixels };
        Vec4 crosshairColor = Vec4 { 1.0f, 1.0f, 1.0f, 0.2f };
        if (appState->collapsingMobIndex != appState->mobs.size) {
            crosshairColor = Vec4 { 1.0f, 0.5f, 0.5f, 1.0f };
        }
        else if (hoverMobIndex != appState->mobs.size) {
            crosshairColor = Vec4 { 1.0f, 1.0f, 1.0f, 0.8f };
        }

        PushSprite((uint32)SpriteId::PIXEL, crosshairPos, crosshairSize, 0.0f, crosshairColor, screenSize,
                   &transientState->frameState.spriteRenderState);
    }

    // ================================================================================================
    // Vulkan rendering ===============================================================================
    // ================================================================================================

    VkCommandBuffer buffer = appState->vulkanAppState.commandBuffer;

    // TODO revisit this. should the platform coordinate something like this in some other way?
    // swapchain image acquisition timings seem to be kind of sloppy tbh, so this might be the best way.
    VkFence fence = appState->vulkanAppState.fence;
    if (vkWaitForFences(vulkanState.window.device, 1, &fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
        LOG_ERROR("vkWaitForFences didn't return success for fence %lu\n", swapchainImageIndex);
    }
    if (vkResetFences(vulkanState.window.device, 1, &fence) != VK_SUCCESS) {
        LOG_ERROR("vkResetFences didn't return success for fence %lu\n", swapchainImageIndex);
    }

    if (vkResetCommandBuffer(buffer, 0) != VK_SUCCESS) {
        LOG_ERROR("vkResetCommandBuffer failed\n");
    }

    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = 0;
    beginInfo.pInheritanceInfo = nullptr;

    if (vkBeginCommandBuffer(buffer, &beginInfo) != VK_SUCCESS) {
        LOG_ERROR("vkBeginCommandBuffer failed\n");
    }

    const VkClearValue clearValues[] = {
        { 0.0f, 0.0f, 0.0f, 1.0f },
        { 1.0f, 0 }
    };

    VkRenderPassBeginInfo renderPassInfo = {};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = vulkanState.swapchain.renderPass;
    renderPassInfo.framebuffer = vulkanState.swapchain.framebuffers[swapchainImageIndex];
    renderPassInfo.renderArea.offset = { 0, 0 };
    renderPassInfo.renderArea.extent = vulkanState.swapchain.extent;
    renderPassInfo.clearValueCount = C_ARRAY_LENGTH(clearValues);
    renderPassInfo.pClearValues = clearValues;

    vkCmdBeginRenderPass(buffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

#if ENABLE_LIGHTMAPPED_MESH
    // Lightmapped meshes
    {
        const Mat4 model = Mat4::one;
        UploadAndSubmitLightmapMeshDrawCommands(vulkanState.window.device, buffer,
                                                appState->vulkanAppState.lightmapMeshPipeline,
                                                model, view, proj);
    }
#endif

    // Meshes
    {
        LinearAllocator allocator(transientState->scratch);
        UploadAndSubmitMeshDrawCommands(vulkanState.window.device, buffer, appState->vulkanAppState.meshPipeline,
                                        transientState->frameState.meshRenderState, view, proj, &allocator);
    }

    // Sprites
    {
        LinearAllocator allocator(transientState->scratch);
        UploadAndSubmitSpriteDrawCommands(vulkanState.window.device, buffer, appState->vulkanAppState.spritePipeline,
                                          transientState->frameState.spriteRenderState, &allocator);
    }

    // Text
    {
        LinearAllocator allocator(transientState->scratch);
        UploadAndSubmitTextDrawCommands(vulkanState.window.device, buffer, appState->vulkanAppState.textPipeline,
                                        transientState->frameState.textRenderState, &allocator);
    }

    vkCmdEndRenderPass(buffer);

    if (vkEndCommandBuffer(buffer) != VK_SUCCESS) {
        LOG_ERROR("vkEndCommandBuffer failed\n");
    }

    const VkSemaphore waitSemaphores[] = { vulkanState.window.imageAvailableSemaphore };
    const VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };

    const VkSemaphore signalSemaphores[] = { vulkanState.window.renderFinishedSemaphore };

    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = C_ARRAY_LENGTH(waitSemaphores);
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &buffer;
    submitInfo.signalSemaphoreCount = C_ARRAY_LENGTH(signalSemaphores);
    submitInfo.pSignalSemaphores = signalSemaphores;

    if (vkQueueSubmit(vulkanState.window.graphicsQueue, 1, &submitInfo, fence) != VK_SUCCESS) {
        LOG_ERROR("Failed to submit draw command buffer\n");
    }

    return true;
}

APP_LOAD_VULKAN_SWAPCHAIN_STATE_FUNCTION(AppLoadVulkanSwapchainState)
{
    LOG_INFO("Loading Vulkan swapchain-dependent app state\n");

    const VulkanWindow& window = vulkanState.window;
    const VulkanSwapchain& swapchain = vulkanState.swapchain;

    VulkanAppState* app = &(GetAppState(memory)->vulkanAppState);
    TransientState* transientState = GetTransientState(memory);
    LinearAllocator allocator(transientState->scratch);

    if (!LoadSpritePipelineSwapchain(window, swapchain, &allocator, &app->spritePipeline)) {
        LOG_ERROR("Failed to load swapchain-dependent Vulkan sprite pipeline\n");
        return false;
    }

    if (!LoadTextPipelineSwapchain(window, swapchain, &allocator, &app->textPipeline)) {
        LOG_ERROR("Failed to load swapchain-dependent Vulkan text pipeline\n");
        return false;
    }

    if (!LoadMeshPipelineSwapchain(window, swapchain, &allocator, &app->meshPipeline)) {
        LOG_ERROR("Failed to load swapchain-dependent Vulkan mesh pipeline\n");
        return false;
    }

#if ENABLE_LIGHTMAPPED_MESH
    if (!LoadLightmapMeshPipelineSwapchain(window, swapchain, &allocator, &app->lightmapMeshPipeline)) {
        LOG_ERROR("Failed to load swapchain-dependent Vulkan lightmap mesh pipeline\n");
        return false;
    }
#endif

    return true;
}

APP_UNLOAD_VULKAN_SWAPCHAIN_STATE_FUNCTION(AppUnloadVulkanSwapchainState)
{
    LOG_INFO("Unloading Vulkan swapchain-dependent app state\n");

    const VkDevice& device = vulkanState.window.device;
    VulkanAppState* app = &(GetAppState(memory)->vulkanAppState);

#if ENABLE_LIGHTMAPPED_MESH
    UnloadLightmapMeshPipelineSwapchain(device, &app->lightmapMeshPipeline);
#endif

    UnloadMeshPipelineSwapchain(device, &app->meshPipeline);

    UnloadTextPipelineSwapchain(device, &app->textPipeline);
    UnloadSpritePipelineSwapchain(device, &app->spritePipeline);
}

APP_LOAD_VULKAN_WINDOW_STATE_FUNCTION(AppLoadVulkanWindowState)
{
    LOG_INFO("Loading Vulkan window-dependent app state\n");

    const VulkanWindow& window = vulkanState.window;

    AppState* appState = GetAppState(memory);
    VulkanAppState* app = &appState->vulkanAppState;
    TransientState* transientState = GetTransientState(memory);
    LinearAllocator allocator(transientState->scratch);

    // Create command pool
    {
        QueueFamilyInfo queueFamilyInfo = GetQueueFamilyInfo(window.surface, window.physicalDevice, &allocator);

        VkCommandPoolCreateInfo poolCreateInfo = {};
        poolCreateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolCreateInfo.queueFamilyIndex = queueFamilyInfo.graphicsFamilyIndex;
        poolCreateInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

        if (vkCreateCommandPool(window.device, &poolCreateInfo, nullptr, &app->commandPool) != VK_SUCCESS) {
            LOG_ERROR("vkCreateCommandPool failed\n");
            return false;
        }
    }

    // Create command buffer
    {
        VkCommandBufferAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = app->commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;

        if (vkAllocateCommandBuffers(window.device, &allocInfo, &app->commandBuffer) != VK_SUCCESS) {
            LOG_ERROR("vkAllocateCommandBuffers failed\n");
            return false;
        }
    }

    // Create fence
    {
        VkFenceCreateInfo fenceCreateInfo = {};
        fenceCreateInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceCreateInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        if (vkCreateFence(window.device, &fenceCreateInfo, nullptr, &app->fence) != VK_SUCCESS) {
            LOG_ERROR("vkCreateFence failed\n");
            return false;
        }
    }

    const bool spritePipeline = LoadSpritePipelineWindow(window, app->commandPool, &allocator, &app->spritePipeline);
    if (!spritePipeline) {
        LOG_ERROR("Failed to load window-dependent Vulkan sprite pipeline\n");
        return false;
    }

    const bool textPipeline = LoadTextPipelineWindow(window, app->commandPool, &allocator, &app->textPipeline);
    if (!textPipeline) {
        LOG_ERROR("Failed to load window-dependent Vulkan text pipeline\n");
        return false;
    }

    const bool meshPipeline = LoadMeshPipelineWindow(window, app->commandPool, &allocator, &app->meshPipeline);
    if (!meshPipeline) {
        LOG_ERROR("Failed to load Vulkan mesh pipeline\n");
        return false;
    }

#if ENABLE_LIGHTMAPPED_MESH
    const bool lightmapMeshPipeline = LoadLightmapMeshPipelineWindow(window, app->commandPool, &allocator,
                                                                     &app->lightmapMeshPipeline);
    if (!lightmapMeshPipeline) {
        LOG_ERROR("Failed to load Vulkan lightmap mesh pipeline\n");
        return false;
    }
#endif

    // Sprites
    {
        const char* spriteFilePaths[] = {
            "data/sprites/pixel.png",
            "data/sprites/jon.png",
            "data/sprites/rock.png"
        };
        static_assert(C_ARRAY_LENGTH(spriteFilePaths) == (uint32)SpriteId::COUNT);

        for (uint32 i = 0; i < C_ARRAY_LENGTH(spriteFilePaths); i++) {
            int width, height, channels;
            unsigned char* imageData = stbi_load(spriteFilePaths[i], &width, &height, &channels, 0);
            if (imageData == NULL) {
                DEBUG_PANIC("Failed to load sprite: %s\n", spriteFilePaths[i]);
            }
            defer(stbi_image_free(imageData));

            uint8* vulkanImageData = (uint8*)imageData;
            if (channels == 3) {
                LOG_ERROR("Image %s with 3 channels, converting to RGBA\n", spriteFilePaths[i]);

                vulkanImageData = allocator.New<uint8>(width * height * 4);
                for (int y = 0; y < height; y++) {
                    for (int x = 0; x < width; x++) {
                        const int imageDataInd = (y * width + x) * 3;
                        vulkanImageData[imageDataInd + 0] = imageData[imageDataInd + 0];
                        vulkanImageData[imageDataInd + 1] = imageData[imageDataInd + 1];
                        vulkanImageData[imageDataInd + 2] = imageData[imageDataInd + 2];
                        vulkanImageData[imageDataInd + 3] = 255;
                    }
                }

                channels = 4;
            }

            VulkanImage sprite;
            if (!LoadVulkanImage(window.device, window.physicalDevice, window.graphicsQueue, app->commandPool,
                                 width, height, channels, vulkanImageData, &sprite)) {
                DEBUG_PANIC("Failed to Vulkan image for sprite %s\n", spriteFilePaths[i]);
            }

            uint32 spriteIndex;
            if (!RegisterSprite(window.device, &app->spritePipeline, sprite, &spriteIndex)) {
                DEBUG_PANIC("Failed to register sprite %s\n", spriteFilePaths[i]);
            }
            DEBUG_ASSERT(spriteIndex == i);
        }
    }

    // Fonts
    {
        struct FontData {
            const_string filePath;
            uint32 height;
        };
        const FontData fontData[] = {
            { ToString("data/fonts/ocr-a/regular.ttf"), 18 },
            { ToString("data/fonts/ocr-a/regular.ttf"), 24 },
        };
        static_assert(C_ARRAY_LENGTH(fontData) == (uint32)FontId::COUNT);

        FT_Library ftLibrary;
        FT_Error error = FT_Init_FreeType(&ftLibrary);
        if (error) {
            DEBUG_PANIC("FreeType init error: %d\n", error);
        }

        for (uint32 i = 0; i < C_ARRAY_LENGTH(fontData); i++) {
            LoadFontFaceResult fontFaceResult;
            if (!LoadFontFace(ftLibrary, fontData[i].filePath, fontData[i].height, &allocator, &fontFaceResult)) {
                DEBUG_PANIC("Failed to load font face at %.*s\n", fontData[i].filePath.size, fontData[i].filePath.data);
            }

            if (!RegisterFont(window.device, window.physicalDevice, window.graphicsQueue, app->commandPool,
                              &app->textPipeline, fontFaceResult, &appState->fontFaces[i])) {
                DEBUG_PANIC("Failed to register font %lu\n", i);
            }
        }
    }

    return true;
}

APP_UNLOAD_VULKAN_WINDOW_STATE_FUNCTION(AppUnloadVulkanWindowState)
{
    LOG_INFO("Unloading Vulkan window-dependent app state\n");

    const VkDevice& device = vulkanState.window.device;
    VulkanAppState* app = &(GetAppState(memory)->vulkanAppState);

#if ENABLE_LIGHTMAPPED_MESH
    UnloadLightmapMeshPipelineWindow(device, &app->lightmapMeshPipeline);
#endif
    UnloadMeshPipelineWindow(device, &app->meshPipeline);

    UnloadTextPipelineWindow(device, &app->textPipeline);
    UnloadSpritePipelineWindow(device, &app->spritePipeline);

    vkDestroyFence(device, app->fence, nullptr);
    vkDestroyCommandPool(device, app->commandPool, nullptr);
}

#include "imgui.cpp"
#include "lightmap.cpp"
#include "mesh.cpp"

#include <km_common/km_array.cpp>
#include <km_common/km_container.cpp>
#include <km_common/km_load_font.cpp>
#include <km_common/km_load_obj.cpp>
#include <km_common/km_memory.cpp>
#include <km_common/km_os.cpp>
#include <km_common/km_string.cpp>

#include <km_common/app/km_app.cpp>
#include <km_common/app/km_input.cpp>
#include <km_common/app/km_log.cpp>

#include <km_common/vulkan/km_vulkan_core.cpp>
#include <km_common/vulkan/km_vulkan_sprite.cpp>
#include <km_common/vulkan/km_vulkan_text.cpp>
#include <km_common/vulkan/km_vulkan_util.cpp>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#undef STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#undef STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_SPRINTF_IMPLEMENTATION
#include <stb_sprintf.h>
#undef STB_SPRINTF_IMPLEMENTATION
