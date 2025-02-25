//
// Copyright (c) 2008-2022 the Urho3D project.
// Copyright (c) 2023-2023 the rbfx project.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

#include "../Precompiled.h"

#include <EASTL/sort.h>

#include "../Core/Context.h"
#include "../Core/WorkQueue.h"
#include "../Graphics/Camera.h"
#include "../Graphics/DebugRenderer.h"
#include "../IO/File.h"
#include "../Graphics/Geometry.h"
#include "../Graphics/Material.h"
#include "../Graphics/Octree.h"
#include "../Graphics/Renderer.h"
#include "../Graphics/VertexBuffer.h"
#include "../Graphics/Zone.h"
#include "../IO/Log.h"
#include "../Scene/Scene.h"

#include "../DebugNew.h"

#ifdef _MSC_VER
#pragma warning(disable:6293)
#endif

namespace Urho3D
{

static const ea::vector<ea::string> giTypeNames = {
    "None",
    "Use LightMap",
    "Blend Light Probes",
};

static const ea::vector<ea::string> reflectionModeNames = {
    "Zone",
    "Nearest Probe",
    "Blend Probes",
    "Blend Probes and Zone",
};

SourceBatch::SourceBatch() = default;

SourceBatch::SourceBatch(const SourceBatch& batch) = default;

SourceBatch::~SourceBatch() = default;

SourceBatch& SourceBatch::operator =(const SourceBatch& rhs)= default;


Drawable::Drawable(Context* context, DrawableFlags drawableFlags) :
    Component(context),
    boundingBox_(0.0f, 0.0f),
    drawableFlags_(drawableFlags),
    worldBoundingBoxDirty_(true),
    castShadows_(false),
    occluder_(false),
    occludee_(true),
    updateQueued_(false),
    zoneDirty_(false),
    octant_(nullptr),
    viewMask_(DEFAULT_VIEWMASK),
    lightMask_(DEFAULT_LIGHTMASK),
    shadowMask_(DEFAULT_SHADOWMASK),
    zoneMask_(DEFAULT_ZONEMASK),
    viewFrameNumber_(0),
    distance_(0.0f),
    lodDistance_(0.0f),
    drawDistance_(0.0f),
    shadowDistance_(0.0f),
    sortValue_(0.0f),
    minZ_(0.0f),
    maxZ_(0.0f),
    lodBias_(1.0f),
    basePassFlags_(0),
    maxLights_(0),
    firstLight_(nullptr)
{
}

Drawable::~Drawable()
{
    RemoveFromOctree();
}

void Drawable::RegisterObject(Context* context)
{
    URHO3D_ATTRIBUTE("Max Lights", int, maxLights_, 0, AM_DEFAULT);
    URHO3D_ATTRIBUTE("View Mask", int, viewMask_, DEFAULT_VIEWMASK, AM_DEFAULT);
    URHO3D_ATTRIBUTE("Light Mask", int, lightMask_, DEFAULT_LIGHTMASK, AM_DEFAULT);
    URHO3D_ATTRIBUTE("Shadow Mask", int, shadowMask_, DEFAULT_SHADOWMASK, AM_DEFAULT);
    URHO3D_ACCESSOR_ATTRIBUTE("Zone Mask", GetZoneMask, SetZoneMask, unsigned, DEFAULT_ZONEMASK, AM_DEFAULT);
    URHO3D_ENUM_ACCESSOR_ATTRIBUTE("Global Illumination", GetGlobalIlluminationType, SetGlobalIlluminationType, GlobalIlluminationType, giTypeNames, GlobalIlluminationType::None, AM_DEFAULT);
    URHO3D_ENUM_ACCESSOR_ATTRIBUTE("Reflection Mode", GetReflectionMode, SetReflectionMode, ReflectionMode, reflectionModeNames, ReflectionMode::BlendProbesAndZone, AM_DEFAULT);
}

void Drawable::OnSetEnabled()
{
    bool enabled = IsEnabledEffective();

    if (enabled && !octant_)
        AddToOctree();
    else if (!enabled && octant_)
        RemoveFromOctree();
}

void Drawable::ProcessCustomRayQuery(const RayOctreeQuery& query, const BoundingBox& worldBoundingBox, ea::vector<RayQueryResult>& results)
{
    const auto distanceAndNormal = query.ray_.HitDistanceAndNormal(worldBoundingBox);
    if (distanceAndNormal.distance_ < query.maxDistance_)
    {
        RayQueryResult result;
        result.position_ = query.ray_.origin_ + distanceAndNormal.distance_ * query.ray_.direction_;
        // For the worldBoundingBox the normal is in world space.
        result.normal_ = distanceAndNormal.normal_;
        result.distance_ = distanceAndNormal.distance_;
        result.drawable_ = this;
        result.node_ = GetNode();
        result.subObject_ = M_MAX_UNSIGNED;
        results.push_back(result);
    }
}

void Drawable::ProcessRayQuery(const RayOctreeQuery& query, ea::vector<RayQueryResult>& results)
{
    ProcessCustomRayQuery(query, GetWorldBoundingBox(), results);
}

void Drawable::UpdateBatches(const FrameInfo& frame)
{
    const BoundingBox& worldBoundingBox = GetWorldBoundingBox();
    const Matrix3x4& worldTransform = node_->GetWorldTransform();
    distance_ = frame.camera_->GetDistance(worldBoundingBox.Center());

    for (unsigned i = 0; i < batches_.size(); ++i)
    {
        batches_[i].distance_ = distance_;
        batches_[i].worldTransform_ = &worldTransform;
    }

    float scale = worldBoundingBox.Size().DotProduct(DOT_SCALE);
    float newLodDistance = frame.camera_->GetLodDistance(distance_, scale, lodBias_);

    if (newLodDistance != lodDistance_)
        lodDistance_ = newLodDistance;
}

Geometry* Drawable::GetLodGeometry(unsigned batchIndex, unsigned level)
{
    // By default return the visible batch geometry
    if (batchIndex < batches_.size())
        return batches_[batchIndex].geometry_;
    else
        return nullptr;
}

bool Drawable::DrawOcclusion(OcclusionBuffer* buffer)
{
    return true;
}

void Drawable::DrawDebugGeometry(DebugRenderer* debug, bool depthTest)
{
    if (debug && IsEnabledEffective())
        debug->AddBoundingBox(GetWorldBoundingBox(), 0x7700ff00_argb, depthTest);
}

void Drawable::SetDrawDistance(float distance)
{
    drawDistance_ = distance;
}

void Drawable::SetShadowDistance(float distance)
{
    shadowDistance_ = distance;
}

void Drawable::SetLodBias(float bias)
{
    lodBias_ = Max(bias, M_EPSILON);
}

void Drawable::SetViewMask(unsigned mask)
{
    viewMask_ = mask;
}

void Drawable::SetLightMask(unsigned mask)
{
    lightMask_ = mask;
    MarkPipelineStateHashDirty();
}

void Drawable::SetShadowMask(unsigned mask)
{
    shadowMask_ = mask;
}

void Drawable::SetZoneMask(unsigned mask)
{
    zoneMask_ = mask;
    // Mark dirty to reset cached zone
    cachedZone_.cacheInvalidationDistanceSquared_ = -1.0f;
    OnMarkedDirty(node_);
}

void Drawable::SetMaxLights(unsigned num)
{
    maxLights_ = num;
}

void Drawable::SetCastShadows(bool enable)
{
    castShadows_ = enable;
}

void Drawable::SetOccluder(bool enable)
{
    occluder_ = enable;
}

void Drawable::SetOccludee(bool enable)
{
    if (enable != occludee_)
    {
        occludee_ = enable;
        // Reinsert to octree to make sure octant occlusion does not erroneously hide this drawable
        if (octant_ && !updateQueued_)
            octant_->GetOctree()->QueueUpdate(this);
    }
}

void Drawable::SetGlobalIlluminationType(GlobalIlluminationType type)
{
    giType_ = type;
    MarkPipelineStateHashDirty();
}

void Drawable::SetReflectionMode(ReflectionMode mode)
{
    reflectionMode_ = mode;
    MarkPipelineStateHashDirty();
}

void Drawable::MarkForUpdate()
{
    if (!updateQueued_ && octant_)
        octant_->GetOctree()->QueueUpdate(this);
}

const BoundingBox& Drawable::GetWorldBoundingBox()
{
    if (worldBoundingBoxDirty_)
    {
        OnWorldBoundingBoxUpdate();
        worldBoundingBoxDirty_ = false;
    }

    return worldBoundingBox_;
}

bool Drawable::IsInView() const
{
    // Note: in headless mode there is no renderer subsystem and no view frustum tests are performed, so return
    // always false in that case
    auto* renderer = GetSubsystem<Renderer>();
    return renderer && viewFrameNumber_ == renderer->GetFrameInfo().frameNumber_ && !viewCameras_.empty();
}

bool Drawable::IsInView(Camera* camera) const
{
    auto* renderer = GetSubsystem<Renderer>();
    return renderer && viewFrameNumber_ == renderer->GetFrameInfo().frameNumber_ && (!camera ||
        viewCameras_.contains(camera));
}

bool Drawable::IsInView(const FrameInfo& frame, bool anyCamera) const
{
    return viewFrameNumber_ == frame.frameNumber_ && (anyCamera || viewCameras_.contains(frame.camera_));
}

unsigned Drawable::GetLightMaskInZone() const
{
    const unsigned zoneLightMask = cachedZone_.zone_ ? cachedZone_.zone_->GetLightMask() : 0xffffffff;
    return zoneLightMask & lightMask_;
}

unsigned Drawable::GetShadowMaskInZone() const
{
    const unsigned zoneShadowMask = cachedZone_.zone_ ? cachedZone_.zone_->GetShadowMask() : 0xffffffff;
    return zoneShadowMask & shadowMask_;
}

Geometry* Drawable::GetGeometryIfNotEmpty(Geometry* geometry)
{
    return (geometry && geometry->GetEffectiveIndexCount() > 0) ? geometry : nullptr;
}

unsigned Drawable::RecalculatePipelineStateHash() const
{
    unsigned hash = 0;
    CombineHash(hash, static_cast<unsigned>(giType_));
    CombineHash(hash, reflectionMode_ >= ReflectionMode::BlendProbes);
    return hash;
}

void Drawable::SetZone(Zone* zone, bool temporary)
{
    if (zone)
        cachedZone_.zone_ = zone;
    else
        cachedZone_ = {};

    // If the zone assignment was temporary (inconclusive) set the dirty flag so that it will be re-evaluated on the next frame
    zoneDirty_ = temporary;
}

void Drawable::SetSortValue(float value)
{
    sortValue_ = value;
}

void Drawable::MarkInView(const FrameInfo& frame)
{
    if (frame.frameNumber_ != viewFrameNumber_)
    {
        viewFrameNumber_ = frame.frameNumber_;
        viewCameras_.resize(1);
        viewCameras_[0] = frame.camera_;
    }
    else
        viewCameras_.push_back(frame.camera_);

    basePassFlags_ = 0;
    firstLight_ = nullptr;
    lights_.clear();
    vertexLights_.clear();
}

void Drawable::MarkInView(unsigned frameNumber)
{
    if (frameNumber != viewFrameNumber_)
    {
        viewFrameNumber_ = frameNumber;
        viewCameras_.clear();
    }
}

void Drawable::LimitLights()
{
    // Maximum lights value 0 means unlimited
    if (!maxLights_ || lights_.size() <= maxLights_)
        return;

    // If more lights than allowed, move to vertex lights and cut the list
    const BoundingBox& box = GetWorldBoundingBox();
    for (unsigned i = 0; i < lights_.size(); ++i)
        lights_[i]->SetIntensitySortValue(box);

    ea::quick_sort(lights_.begin(), lights_.end(), CompareDrawables);
    vertexLights_.insert(vertexLights_.end(), lights_.begin() + maxLights_, lights_.end());
    lights_.resize(maxLights_);
}

void Drawable::LimitVertexLights(bool removeConvertedLights)
{
    if (removeConvertedLights)
    {
        for (unsigned i = vertexLights_.size() - 1; i < vertexLights_.size(); --i)
        {
            if (!vertexLights_[i]->GetPerVertex())
                vertexLights_.erase_at(i);
        }
    }

    if (vertexLights_.size() <= MAX_VERTEX_LIGHTS)
        return;

    const BoundingBox& box = GetWorldBoundingBox();
    for (unsigned i = 0; i < vertexLights_.size(); ++i)
        vertexLights_[i]->SetIntensitySortValue(box);

    ea::quick_sort(vertexLights_.begin(), vertexLights_.end(), CompareDrawables);
    vertexLights_.resize(MAX_VERTEX_LIGHTS);
}

void Drawable::OnNodeSet(Node* previousNode, Node* currentNode)
{
    if (node_)
        node_->AddListener(this);
}

void Drawable::OnSceneSet(Scene* scene)
{
    if (scene)
        AddToOctree();
    else
        RemoveFromOctree();
}

void Drawable::OnMarkedDirty(Node* node)
{
    worldBoundingBoxDirty_ = true;
    if (!updateQueued_ && octant_)
        octant_->GetOctree()->QueueUpdate(this);

    // Mark zone assignment dirty when transform changes
    if (node == node_)
        zoneDirty_ = true;
}

void Drawable::AddToOctree()
{
    // Do not add to octree when disabled
    if (!IsEnabledEffective())
        return;

    Scene* scene = GetScene();
    if (scene)
    {
        auto* octree = scene->GetComponent<Octree>();
        if (octree)
            octree->AddDrawable(this);
        else
            URHO3D_LOGERROR("No Octree component in scene, drawable will not render");
    }
    else
    {
        // We have a mechanism for adding detached nodes to an octree manually, so do not log this error
        //URHO3D_LOGERROR("Node is detached from scene, drawable will not render");
    }
}

void Drawable::RemoveFromOctree()
{
    if (octant_)
    {
        Octree* octree = octant_->GetOctree();
        if (updateQueued_)
            octree->CancelUpdate(this);

        // Perform subclass specific deinitialization if necessary
        OnRemoveFromOctree();

        octree->RemoveDrawable(this, octant_);
    }
}

void Drawable::RequestUpdateBatchesDelayed(const FrameInfo& frame)
{
    auto workQueue = context_->GetSubsystem<WorkQueue>();
    workQueue->PostTaskForMainThread([this, &frame]()
    {
        UpdateBatchesDelayed(frame);
    }, TaskPriority::Immediate);
}

bool WriteDrawablesToOBJ(const ea::vector<Drawable*>& drawables, File* outputFile, bool asZUp, bool asRightHanded, bool writeLightmapUV)
{
    // Must track indices independently to deal with potential mismatching of drawables vertex attributes (ie. one with UV, another without, then another with)
    unsigned currentPositionIndex = 1;
    unsigned currentUVIndex = 1;
    unsigned currentNormalIndex = 1;
    bool anythingWritten = false;

    // Write the common "I came from X" comment
    outputFile->WriteLine("# OBJ file exported from Urho3D");

    for (unsigned i = 0; i < drawables.size(); ++i)
    {
        Drawable* drawable = drawables[i];

        // Only write enabled drawables
        if (!drawable->IsEnabledEffective())
            continue;

        Node* node = drawable->GetNode();
        Matrix3x4 transMat = drawable->GetNode()->GetWorldTransform();
        Matrix3x4 n = transMat.Inverse();
        Matrix3 normalMat = Matrix3(n.m00_, n.m01_, n.m02_, n.m10_, n.m11_, n.m12_, n.m20_, n.m21_, n.m22_);
        normalMat = normalMat.Transpose();

        const ea::vector<SourceBatch>& batches = drawable->GetBatches();
        for (unsigned geoIndex = 0; geoIndex < batches.size(); ++geoIndex)
        {
            Geometry* geo = drawable->GetLodGeometry(geoIndex, 0);
            if (geo == nullptr)
                continue;
            if (geo->GetPrimitiveType() != TRIANGLE_LIST)
            {
                URHO3D_LOGERRORF("%s (%u) %s (%u) Geometry %u contains an unsupported geometry type %u",
                    node->GetName().length() > 0 ? node->GetName().c_str() : "Node", node->GetID(), drawable->GetTypeName().c_str(), drawable->GetID(), geoIndex, (unsigned)geo->GetPrimitiveType());
                continue;
            }

            // If we've reached here than we're going to actually write something to the OBJ file
            anythingWritten = true;

            const unsigned char* vertexData;
            const unsigned char* indexData;
            unsigned elementSize, indexSize;
            const ea::vector<VertexElement>* elements;
            geo->GetRawData(vertexData, elementSize, indexData, indexSize, elements);
            if (!vertexData || !elements)
                continue;

            bool hasPosition = VertexBuffer::HasElement(*elements, TYPE_VECTOR3, SEM_POSITION);
            if (!hasPosition)
            {
                URHO3D_LOGERRORF("%s (%u) %s (%u) Geometry %u contains does not have Vector3 type positions in vertex data",
                    node->GetName().length() > 0 ? node->GetName().c_str() : "Node", node->GetID(), drawable->GetTypeName().c_str(), drawable->GetID(), geoIndex);
                continue;
            }

            bool hasNormals = VertexBuffer::HasElement(*elements, TYPE_VECTOR3, SEM_NORMAL);
            bool hasUV = VertexBuffer::HasElement(*elements, TYPE_VECTOR2, SEM_TEXCOORD, 0);
            bool hasLMUV = VertexBuffer::HasElement(*elements, TYPE_VECTOR2, SEM_TEXCOORD, 1);

            if (elementSize > 0 && indexSize > 0)
            {
                unsigned vertexStart = geo->GetVertexStart();
                unsigned vertexCount = geo->GetVertexCount();
                unsigned indexStart = geo->GetIndexStart();
                unsigned indexCount = geo->GetIndexCount();

                // Name NodeID DrawableType DrawableID GeometryIndex ("Geo" is included for clarity as StaticModel_32_2 could easily be misinterpreted or even quickly misread as 322)
                // Generated object name example: Node_5_StaticModel_32_Geo_0 ... or ... Bob_5_StaticModel_32_Geo_0
                outputFile->WriteLine(ea::string("o ").append_sprintf("%s_%u_%s_%u_Geo_%u",
                    node->GetName().length() > 0 ? node->GetName().c_str() : "Node", node->GetID(),
                    drawable->GetTypeName().c_str(), drawable->GetID(), geoIndex));

                // Write vertex position
                unsigned positionOffset = VertexBuffer::GetElementOffset(*elements, TYPE_VECTOR3, SEM_POSITION);
                for (unsigned j = 0; j < vertexCount; ++j)
                {
                    Vector3 vertexPosition = *((const Vector3*)(&vertexData[(vertexStart + j) * elementSize + positionOffset]));
                    vertexPosition = transMat * vertexPosition;

                    // Convert coordinates as requested
                    if (asRightHanded)
                        vertexPosition.x_ *= -1;
                    if (asZUp)
                    {
                        float yVal = vertexPosition.y_;
                        vertexPosition.y_ = vertexPosition.z_;
                        vertexPosition.z_ = yVal;
                    }
                    outputFile->WriteLine("v " + vertexPosition.ToString());
                }

                if (hasNormals)
                {
                    unsigned normalOffset = VertexBuffer::GetElementOffset(*elements, TYPE_VECTOR3, SEM_NORMAL);
                    for (unsigned j = 0; j < vertexCount; ++j)
                    {
                        Vector3 vertexNormal = *((const Vector3*)(&vertexData[(vertexStart + j) * elementSize + normalOffset]));
                        vertexNormal = normalMat * vertexNormal;
                        vertexNormal.Normalize();

                        if (asRightHanded)
                            vertexNormal.x_ *= -1;
                        if (asZUp)
                        {
                            float yVal = vertexNormal.y_;
                            vertexNormal.y_ = vertexNormal.z_;
                            vertexNormal.z_ = yVal;
                        }

                        outputFile->WriteLine("vn " + vertexNormal.ToString());
                    }
                }

                // Write TEXCOORD1 or TEXCOORD2 if it was chosen
                if (hasUV || (hasLMUV && writeLightmapUV))
                {
                    // if writing Lightmap UV is chosen, only use it if TEXCOORD2 exists, otherwise use TEXCOORD1
                    unsigned texCoordOffset = (writeLightmapUV && hasLMUV) ? VertexBuffer::GetElementOffset(*elements, TYPE_VECTOR2, SEM_TEXCOORD, 1) :
                        VertexBuffer::GetElementOffset(*elements, TYPE_VECTOR2, SEM_TEXCOORD, 0);
                    for (unsigned j = 0; j < vertexCount; ++j)
                    {
                        Vector2 uvCoords = *((const Vector2*)(&vertexData[(vertexStart + j) * elementSize + texCoordOffset]));
                        outputFile->WriteLine("vt " + uvCoords.ToString());
                    }
                }

                // If we don't have UV but have normals then must write a double-slash to indicate the absence of UV coords, otherwise use a single slash
                const ea::string slashCharacter = hasNormals ? "//" : "/";

                // Amount by which to offset indices in the OBJ vs their values in the Urho3D geometry, basically the lowest index value
                // Compensates for the above vertex writing which doesn't write ALL vertices, just the used ones
                unsigned indexOffset = M_MAX_INT;
                for (unsigned indexIdx = indexStart; indexIdx < indexStart + indexCount; indexIdx++)
                {
                    if (indexSize == 2)
                        indexOffset = Min(indexOffset, (unsigned)*((unsigned short*)(indexData + indexIdx * indexSize)));
                    else
                        indexOffset = Min(indexOffset, *((unsigned*)(indexData + indexIdx * indexSize)));
                }

                for (unsigned indexIdx = indexStart; indexIdx < indexStart + indexCount; indexIdx += 3)
                {
                    // Deal with 16 or 32 bit indices
                    unsigned longIndices[3];
                    if (indexSize == 2)
                    {
                        //16 bit indices
                        unsigned short indices[3];
                        memcpy(indices, indexData + (indexIdx * indexSize), (size_t)indexSize * 3);
                        longIndices[0] = indices[0] - indexOffset;
                        longIndices[1] = indices[1] - indexOffset;
                        longIndices[2] = indices[2] - indexOffset;
                    }
                    else
                    {
                        //32 bit indices
                        unsigned indices[3];
                        memcpy(indices, indexData + (indexIdx * indexSize), (size_t)indexSize * 3);
                        longIndices[0] = indices[0] - indexOffset;
                        longIndices[1] = indices[1] - indexOffset;
                        longIndices[2] = indices[2] - indexOffset;
                    }

                    ea::string output = "f ";
                    if (hasNormals)
                    {
                        output.append_sprintf("%u/%u/%u %u/%u/%u %u/%u/%u", currentPositionIndex + longIndices[0],
                            currentUVIndex + longIndices[0], currentNormalIndex + longIndices[0],
                            currentPositionIndex + longIndices[1], currentUVIndex + longIndices[1],
                            currentNormalIndex + longIndices[1], currentPositionIndex + longIndices[2],
                            currentUVIndex + longIndices[2], currentNormalIndex + longIndices[2]);
                    }
                    else if (hasNormals || hasUV)
                    {
                        unsigned secondTraitIndex = hasNormals ? currentNormalIndex : currentUVIndex;
                        output.append_sprintf("%u%s%u %u%s%u %u%s%u", currentPositionIndex + longIndices[0],
                            slashCharacter.c_str(), secondTraitIndex + longIndices[0],
                            currentPositionIndex + longIndices[1], slashCharacter.c_str(),
                            secondTraitIndex + longIndices[1], currentPositionIndex + longIndices[2],
                            slashCharacter.c_str(), secondTraitIndex + longIndices[2]);
                    }
                    else
                    {
                        output.append_sprintf("%u %u %u", currentPositionIndex + longIndices[0],
                            currentPositionIndex + longIndices[1], currentPositionIndex + longIndices[2]);
                    }
                    outputFile->WriteLine(output);
                }

                // Increment our positions based on what vertex attributes we have
                currentPositionIndex += vertexCount;
                currentNormalIndex += hasNormals ? vertexCount : 0;
                // is it possible to have TEXCOORD2 but not have TEXCOORD1, assume anything
                currentUVIndex += (hasUV || hasLMUV) ? vertexCount : 0;
            }
        }
    }
    return anythingWritten;
}

}
