#include "MeshBender.h"
#include "Localization.h"

#include <UnigineMaterials.h>
#include <UnigineNodes.h>
#include <UnigineObjects.h>

#include <QHash>
#include <QPair>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <functional>

using namespace Unigine;

namespace ArtistTool
{

namespace
{

// A unit vector along an axis of the asset.
Math::vec3 axis_vector(AxisDirection axis)
{
	Math::vec3 v(0.0f, 0.0f, 0.0f);
	v[int(axis) / 2] = int(axis) % 2 ? -1.0f : 1.0f;
	return v;
}

// The axes of the source: the one that goes along the curve, the one to its right and
// the one up. An up axis along the forward one is taken as the next axis.
void axis_vectors(const BendAxes &axes, Math::vec3 &forward, Math::vec3 &right, Math::vec3 &up)
{
	forward = axis_vector(axes.forward);
	AxisDirection up_axis = axes.up;
	if (int(up_axis) / 2 == int(axes.forward) / 2)
		up_axis = AxisDirection((int(axes.forward) / 2 * 2 + 2) % 6);
	up = axis_vector(up_axis);
	right = Math::cross(forward, up);
}

// The objects of a node and of everything in it, the content of node references too.
// A disabled node is not drawn in the asset, and neither is anything in it.
void collect_objects(const NodePtr &node, QVector<ObjectMeshStaticPtr> &objects)
{
	if (!node || !node->isEnabled())
		return;
	if (node->getType() == Node::OBJECT_MESH_STATIC)
		objects.append(checked_ptr_cast<ObjectMeshStatic>(node));
	else if (node->getType() == Node::NODE_REFERENCE)
		collect_objects(checked_ptr_cast<NodeReference>(node)->getReference(), objects);
	for (int i = 0, num = node->getNumChildren(); i < num; ++i)
		collect_objects(node->getChild(i), objects);
}

} // namespace

int BendSource::numTriangles() const
{
	int triangles = 0;
	for (const Surface &surface : surfaces)
		triangles += int(surface.indices.size()) / 3;
	return triangles;
}

bool BendSource::load(const PlaceableAsset &asset, BendSource &source, QString &error)
{
	source = BendSource();
	NodePtr node = createNode(asset);
	if (!node)
	{
		error = uiText("The asset could not be loaded.", "Не удалось загрузить ассет.");
		return false;
	}

	QVector<ObjectMeshStaticPtr> objects;
	collect_objects(node, objects);
	const Math::Mat4 root_inverse = Math::inverse(node->getWorldTransform());

	for (int object_index = 0; object_index < objects.size(); ++object_index)
	{
		const ObjectMeshStaticPtr &object = objects[object_index];
		const ConstMeshPtr mesh = object->getMeshForceRAM();
		if (!mesh)
			continue;

		// Into the space of the asset: the object may be moved, turned and scaled in it.
		const Math::mat4 transform = Math::mat4(root_inverse * object->getWorldTransform());
		const Math::mat3 rotation = Math::mat3(Math::transpose(Math::inverse(transform)));
		// A mirrored object (a negative scale) turns its triangles inside out: their
		// corners go the other way round to face out again.
		const Math::vec3 x = transform.getColumn3(0);
		const Math::vec3 y = transform.getColumn3(1);
		const Math::vec3 z = transform.getColumn3(2);
		const bool mirrored = Math::dot(x, Math::cross(y, z)) < 0.0f;

		for (int s = 0, num = qMin(mesh->getNumSurfaces(), object->getNumSurfaces()); s < num; ++s)
		{
			// Not what is neither drawn nor casts shadows.
			const bool casts = (object->getCastShadow(s) || object->getCastWorldShadow(s)) && object->getShadowMask(s) != 0;
			if (!object->isEnabled(s) || (object->getViewportMask(s) == 0 && !casts))
				continue;
			const int corners = mesh->getNumCIndices(s);
			if (corners < 3 || corners != mesh->getNumTIndices(s))
				continue;

			Surface out;
			out.object = object_index;
			// Every surface by its own name: the copies find their surfaces in the
			// result by it.
			const QString name = QString::fromUtf8(mesh->getSurfaceName(s));
			out.name = name;
			for (int n = 2; std::any_of(source.surfaces.begin(), source.surfaces.end(),
				[&out](const Surface &other) { return other.name == out.name; }); ++n)
				out.name = QString::fromUtf8("%1_%2").arg(name).arg(n);
			if (const MaterialPtr material = object->getMaterial(s))
				out.material = material->getGUID();
			out.min_visible = object->getMinVisibleDistance(s);
			out.max_visible = object->getMaxVisibleDistance(s);
			out.min_fade = object->getMinFadeDistance(s);
			out.max_fade = object->getMaxFadeDistance(s);
			out.viewport_mask = object->getViewportMask(s);
			out.shadow_mask = object->getShadowMask(s);
			out.cast_shadow = object->getCastShadow(s);
			out.cast_world_shadow = object->getCastWorldShadow(s);
			static const QRegularExpression shadow_suffix(QString::fromUtf8("shadow_?lod_?\\d*$"),
				QRegularExpression::CaseInsensitiveOption);
			out.shadow_lod = shadow_suffix.match(name).hasMatch();
			static const QRegularExpression lod_suffix(QString::fromUtf8("_lod_?(\\d+)$"),
				QRegularExpression::CaseInsensitiveOption);
			const QRegularExpressionMatch match = lod_suffix.match(name);
			if (match.hasMatch() && !out.shadow_lod)
				out.lod = match.captured(1).toInt();

			const int num_vertex = mesh->getNumVertex(s);
			const int num_tvertex = mesh->getNumTVertex(s);
			const int num_uv0 = mesh->getNumTexCoords0(s);
			const int num_uv1 = mesh->getNumTexCoords1(s);
			const int num_normals = mesh->getNumNormals(s);
			const int num_tangents = mesh->getNumTangents(s);
			const int num_colors = mesh->getNumColors(s);

			// A vertex of the result for every pair of a position and a texture vertex the
			// triangles use: what they share stays shared.
			QHash<QPair<int, int>, int> vertices;
			for (int k = 0; k < corners; ++k)
			{
				const int ci = mesh->getCIndex(k, s);
				const int ti = mesh->getTIndex(k, s);
				const QPair<int, int> key(ci, ti);
				auto it = vertices.constFind(key);
				if (it == vertices.constEnd())
				{
					const int index = int(out.positions.size());
					it = vertices.insert(key, index);

					out.positions.append(transform * mesh->getVertex(ci, s));
					out.uv0.append(ti < num_uv0 ? mesh->getTexCoord0(ti, s) : Math::vec2(0.0f, 0.0f));
					out.uv1.append(ti < num_uv1 ? mesh->getTexCoord1(ti, s) : out.uv0.last());

					// A loaded mesh keeps its normals in its tangents (a quaternion of the
					// whole basis per texture vertex); a separate list of normals it may
					// not have at all.
					Math::vec3 normal(0.0f, 0.0f, 1.0f);
					if (ti < num_tangents)
						normal = mesh->getTangent(ti, s).getNormal();
					else if (num_normals == num_tvertex && ti < num_normals)
						normal = mesh->getNormal(ti, s);
					else if (ci < num_normals)
						normal = mesh->getNormal(ci, s);
					normal = rotation * normal;
					if (Math::length2(normal) > 1e-12f)
						normal = Math::normalize(normal);
					out.normals.append(normal);

					Math::vec4 color(1.0f, 1.0f, 1.0f, 1.0f);
					if (num_colors == num_tvertex && ti < num_colors)
						color = mesh->getColor(ti, s);
					else if (num_colors == num_vertex && ci < num_colors)
						color = mesh->getColor(ci, s);
					out.colors.append(color);
				}
				out.indices.append(*it);
			}
			if (mirrored)
			{
				for (int k = 0; k + 2 < out.indices.size(); k += 3)
					std::swap(out.indices[k + 1], out.indices[k + 2]);
			}
			source.surfaces.append(out);
		}
	}
	node.deleteLater();

	// The rules go object by object: one object of the asset may have its LODs or its
	// shadow LOD set up and another one not.
	QVector<int> object_list;
	for (const Surface &surface : source.surfaces)
	{
		if (!object_list.contains(surface.object))
			object_list.append(surface.object);
	}
	QVector<bool> keep(source.surfaces.size(), true);
	bool lods_cut = false;
	bool shadows_set = false;
	for (int object : object_list)
	{
		// LODs told by their names but not by the distances (a bare .mesh, or an object
		// whose LODs were never set up): they would all be seen at once, so only the
		// first one of the object is taken.
		bool named_lods = false;
		bool distances = false;
		int first_lod = -1;
		for (const Surface &surface : source.surfaces)
		{
			if (surface.object != object)
				continue;
			named_lods = named_lods || surface.lod > 0;
			distances = distances || surface.min_visible > 0.0f || surface.max_visible < Math::Consts::INF;
			if (surface.lod >= 0 && (first_lod < 0 || surface.lod < first_lod))
				first_lod = surface.lod;
		}
		if (named_lods && !distances)
		{
			for (int i = 0; i < source.surfaces.size(); ++i)
			{
				const Surface &surface = source.surfaces[i];
				if (surface.object == object && surface.lod >= 0 && surface.lod != first_lod)
					keep[i] = false;
			}
			lods_cut = true;
		}

		// A shadow LOD told only by its name, still drawn like the others: it is made to
		// only cast the shadow, and the other surfaces of its object to not cast it.
		bool shadow_named = false;
		for (const Surface &surface : source.surfaces)
			shadow_named = shadow_named || (surface.object == object && surface.shadow_lod && surface.viewport_mask != 0);
		if (shadow_named)
		{
			for (Surface &surface : source.surfaces)
			{
				if (surface.object != object)
					continue;
				if (surface.shadow_lod)
				{
					surface.viewport_mask = 0;
					surface.cast_shadow = true;
					surface.cast_world_shadow = true;
				} else
				{
					surface.cast_shadow = false;
					surface.cast_world_shadow = false;
				}
			}
			shadows_set = true;
		}
	}
	QVector<Surface> kept;
	for (int i = 0; i < source.surfaces.size(); ++i)
	{
		if (keep[i])
			kept.append(source.surfaces[i]);
	}
	source.surfaces = kept;
	QStringList notes;
	if (lods_cut)
		notes.append(uiText("LODs with no visible distances: only the first one is taken",
			"LOD-ы без дистанций видимости: взят только первый"));
	if (shadows_set)
		notes.append(uiText("the shadow LOD only casts the shadow, the other surfaces of its object do not",
			"shadow LOD только отбрасывает тень, остальные поверхности его объекта - нет"));
	source.note = notes.join(QString::fromUtf8("; "));

	if (source.isEmpty())
	{
		error = uiText("The asset has no meshes to bend.", "В ассете нет мешей, которые можно изогнуть.");
		return false;
	}
	return true;
}

BendBounds bendBounds(const BendSource &source, const BendAxes &axes)
{
	Math::vec3 forward, right, up;
	axis_vectors(axes, forward, right, up);

	BendBounds bounds;
	bool first = true;
	for (const BendSource::Surface &surface : source.surfaces)
	{
		for (const Math::vec3 &p : surface.positions)
		{
			const Math::vec3 scaled = p * axes.scale;
			const float a = Math::dot(scaled, forward);
			const float r = Math::dot(scaled, right);
			const float u = Math::dot(scaled, up);
			if (first)
			{
				bounds.along_min = bounds.along_max = a;
				bounds.right_min = bounds.right_max = r;
				bounds.up_min = bounds.up_max = u;
				first = false;
				continue;
			}
			bounds.up_min = qMin(bounds.up_min, u);
			bounds.up_max = qMax(bounds.up_max, u);
			bounds.along_min = qMin(bounds.along_min, a);
			bounds.along_max = qMax(bounds.along_max, a);
			bounds.right_min = qMin(bounds.right_min, r);
			bounds.right_max = qMax(bounds.right_max, r);
		}
	}
	return bounds;
}

void bendCopy(const BendSource &source, const BendAxes &axes, const BendBounds &bounds, const QStringList &names,
	double start, double fit, const std::function<BendFrame(double)> &frame, const Math::Vec3 &pivot, const MeshPtr &mesh)
{
	Math::vec3 forward, right, up;
	axis_vectors(axes, forward, right, up);
	// The normals of a mesh scaled unevenly turn by the inverse of the scale.
	const Math::vec3 normal_scale(1.0f / axes.scale.x, 1.0f / axes.scale.y, 1.0f / axes.scale.z);

	for (int i = 0; i < source.surfaces.size() && i < names.size(); ++i)
	{
		const BendSource::Surface &surface = source.surfaces[i];
		const QByteArray name = names[i].toUtf8();
		int s = mesh->findSurface(name.constData());
		if (s < 0)
			s = mesh->addSurface(name.constData());
		const int base = mesh->getNumVertex(s);
		for (int v = 0; v < surface.positions.size(); ++v)
		{
			const Math::vec3 p = surface.positions[v] * axes.scale;
			// Where the vertex is along the copy, and so along the curve.
			const double along = double(Math::dot(p, forward)) - double(bounds.along_min);
			const BendFrame f = frame(start + along * fit);
			const float x = Math::dot(p, right) * f.width;
			const float z = Math::dot(p, up);
			const Math::Vec3 position = f.position + Math::Vec3(f.right * x + f.up * z);
			mesh->addVertex(Math::vec3(position - pivot), s);

			const Math::vec3 n = surface.normals[v] * normal_scale;
			// The inverse transpose of the stretch: the width to the right, the fit along
			// the curve, and the width changing along it - a side wall of a copy that
			// gets wider leans out, and its normal leans back.
			const float width = qMax(f.width, 1e-4f);
			const float n_right = Math::dot(n, right);
			Math::vec3 normal = f.right * (n_right / width)
				+ f.forward * (Math::dot(n, forward) / float(qMax(fit, 1e-4)) - n_right * Math::dot(p, right) * f.width_slope / width)
				+ f.up * Math::dot(n, up);
			if (Math::length2(normal) > 1e-12f)
				normal = Math::normalize(normal);
			mesh->addNormal(normal, s);
			mesh->addTexCoord0(surface.uv0[v], s);
			mesh->addTexCoord1(surface.uv1[v], s);
			mesh->addColor(surface.colors[v], s);
		}
		for (int index : surface.indices)
			mesh->addIndex(base + index, s);
	}
}

void applySurface(const ObjectPtr &object, int surface, const BendSource::Surface &source)
{
	if (const MaterialPtr material = Materials::findMaterialByGUID(source.material))
		object->setMaterial(material, surface);
	object->setMinVisibleDistance(source.min_visible, surface);
	object->setMaxVisibleDistance(source.max_visible, surface);
	object->setMinFadeDistance(source.min_fade, surface);
	object->setMaxFadeDistance(source.max_fade, surface);
	object->setViewportMask(source.viewport_mask, surface);
	object->setShadowMask(source.shadow_mask, surface);
	object->setCastShadow(source.cast_shadow, surface);
	object->setCastWorldShadow(source.cast_world_shadow, surface);
}

} // namespace ArtistTool
