#include "MeshBender.h"
#include "Localization.h"

#include <UnigineFileSystem.h>
#include <UnigineMaterials.h>
#include <UnigineNodes.h>
#include <UnigineObjects.h>

#include <QCryptographicHash>
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
// A disabled node is not drawn in the asset, and neither is anything in it. The files of
// the node references and of the meshes are added to 'files' (see BendSource::files).
void collect_objects(const NodePtr &node, QVector<ObjectMeshStaticPtr> &objects, QStringList &files)
{
	if (!node || !node->isEnabled())
		return;
	if (node->getType() == Node::OBJECT_MESH_STATIC)
	{
		const ObjectMeshStaticPtr object = checked_ptr_cast<ObjectMeshStatic>(node);
		objects.append(object);
		files.append(bendSourceFile(object->getMeshPath()));
	} else if (node->getType() == Node::NODE_REFERENCE)
	{
		const NodeReferencePtr reference = checked_ptr_cast<NodeReference>(node);
		files.append(bendSourceFile(reference->getNodePath()));
		collect_objects(reference->getReference(), objects, files);
	}
	for (int i = 0, num = node->getNumChildren(); i < num; ++i)
		collect_objects(node->getChild(i), objects, files);
}

// A vertex of a surface with all it carries, and where it is along the forward axis.
struct SliceVertex
{
	Math::vec3 position;
	Math::vec3 normal;
	Math::vec2 uv0;
	Math::vec2 uv1;
	Math::vec4 color;
	float along{0.0f};
};

SliceVertex lerp_vertex(const SliceVertex &a, const SliceVertex &b, float t)
{
	SliceVertex v;
	v.position = a.position + (b.position - a.position) * t;
	v.normal = a.normal + (b.normal - a.normal) * t;
	if (Math::length2(v.normal) > 1e-12f)
		v.normal = Math::normalize(v.normal);
	v.uv0 = a.uv0 + (b.uv0 - a.uv0) * t;
	v.uv1 = a.uv1 + (b.uv1 - a.uv1) * t;
	v.color = a.color + (b.color - a.color) * t;
	v.along = a.along + (b.along - a.along) * t;
	return v;
}

// The part of the polygon on one side of the plane 'along = cut': the side above it, or
// the one below.
QVector<SliceVertex> clip_polygon(const QVector<SliceVertex> &polygon, float cut, bool above)
{
	QVector<SliceVertex> result;
	for (int i = 0; i < polygon.size(); ++i)
	{
		const SliceVertex &a = polygon[i];
		const SliceVertex &b = polygon[(i + 1) % polygon.size()];
		const bool a_in = above ? a.along >= cut : a.along <= cut;
		const bool b_in = above ? b.along >= cut : b.along <= cut;
		if (a_in)
			result.append(a);
		if (a_in != b_in)
			result.append(lerp_vertex(a, b, (cut - a.along) / (b.along - a.along)));
	}
	return result;
}

// Cuts the triangles of the surface by the planes across the forward axis at the 'cuts'
// (sorted, along that axis) into 'result' (only its geometry is set): every part between
// two planes becomes triangles of its own, with its winding kept. A triangle no plane
// crosses stays as it is, its vertices shared as they were.
void slice_surface(const BendSource::Surface &surface, const QVector<float> &cuts,
	const std::function<float(const Math::vec3 &)> &along_of, BendSource::Surface &result)
{
	// Cleared, not freed: the next copy fills the same memory.
	result.positions.clear();
	result.normals.clear();
	result.uv0.clear();
	result.uv1.clear();
	result.colors.clear();
	result.indices.clear();

	const int count = int(surface.positions.size());
	QVector<float> along(count);
	for (int i = 0; i < count; ++i)
		along[i] = along_of(surface.positions[i]);
	QVector<int> kept(count, -1);
	const auto keep = [&](int i) {
		if (kept[i] < 0)
		{
			result.positions.append(surface.positions[i]);
			result.normals.append(surface.normals[i]);
			result.uv0.append(surface.uv0[i]);
			result.uv1.append(surface.uv1[i]);
			result.colors.append(surface.colors[i]);
			kept[i] = int(result.positions.size()) - 1;
		}
		return kept[i];
	};
	const auto vertex = [&](int i) {
		SliceVertex v;
		v.position = surface.positions[i];
		v.normal = surface.normals[i];
		v.uv0 = surface.uv0[i];
		v.uv1 = surface.uv1[i];
		v.color = surface.colors[i];
		v.along = along[i];
		return v;
	};
	const auto append = [&](const SliceVertex &v) {
		result.positions.append(v.position);
		result.normals.append(v.normal);
		result.uv0.append(v.uv0);
		result.uv1.append(v.uv1);
		result.colors.append(v.color);
		return int(result.positions.size()) - 1;
	};

	QVector<float> bounds;
	for (int t = 0; t + 2 < surface.indices.size(); t += 3)
	{
		const int i0 = surface.indices[t];
		const int i1 = surface.indices[t + 1];
		const int i2 = surface.indices[t + 2];
		const float low = qMin(along[i0], qMin(along[i1], along[i2]));
		const float high = qMax(along[i0], qMax(along[i1], along[i2]));
		// The first cut past its start: none, or past its end - no plane crosses it.
		const auto first_cut = std::upper_bound(cuts.begin(), cuts.end(), low);
		if (first_cut == cuts.end() || *first_cut >= high)
		{
			result.indices.append(keep(i0));
			result.indices.append(keep(i1));
			result.indices.append(keep(i2));
			continue;
		}
		bounds.clear();
		bounds.append(low);
		for (auto cut = first_cut; cut != cuts.end() && *cut < high; ++cut)
			bounds.append(*cut);
		bounds.append(high);
		const QVector<SliceVertex> triangle = {vertex(i0), vertex(i1), vertex(i2)};
		for (int b = 0; b + 1 < bounds.size(); ++b)
		{
			const QVector<SliceVertex> part = clip_polygon(clip_polygon(triangle, bounds[b], true), bounds[b + 1], false);
			if (part.size() < 3)
				continue;
			const int first = append(part[0]);
			int previous = append(part[1]);
			for (int k = 2; k < part.size(); ++k)
			{
				const int current = append(part[k]);
				result.indices.append(first);
				result.indices.append(previous);
				result.indices.append(current);
				previous = current;
			}
		}
	}
}

} // namespace

int BendSource::numTriangles() const
{
	int triangles = 0;
	for (const Surface &surface : surfaces)
		triangles += int(surface.indices.size()) / 3;
	return triangles;
}

QString bendSourceFile(const char *path)
{
	if (String::isEmpty(path))
		return QString();
	// A runtime of an imported asset ("guid://..." of a mesh of an .fbx) is told by the
	// asset it comes from: that is the file that changes when it is reimported.
	const UGUID guid = FileSystem::getGUID(path);
	String virtual_path;
	if (guid.isValid())
	{
		const UGUID asset = FileSystemAssets::resolveAsset(guid);
		virtual_path = FileSystem::getVirtualPath(asset.isValid() ? asset : guid);
	}
	if (virtual_path.empty())
		virtual_path = path;
	return QString::fromUtf8(virtual_path.get()).replace(QLatin1Char('\\'), QLatin1Char('/')).toLower();
}

bool BendSource::load(const PlaceableAsset &asset, BendSource &source, QString &error)
{
	source = BendSource();
	// The asset itself first: a source that can not be loaded is to be tried again when
	// its file changes too.
	source.files.append(bendSourceFile(asset.path.get()));
	NodePtr node = createNode(asset);
	if (!node)
	{
		error = uiText("The asset could not be loaded.", "Не удалось загрузить ассет.");
		return false;
	}

	QVector<ObjectMeshStaticPtr> objects;
	collect_objects(node, objects, source.files);
	source.files.removeAll(QString());
	source.files.removeDuplicates();
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

	QCryptographicHash hash(QCryptographicHash::Md5);
	const auto add = [&hash](const void *data, qsizetype size) {
		hash.addData(QByteArrayView(static_cast<const char *>(data), size));
	};
	for (const Surface &surface : source.surfaces)
	{
		hash.addData(surface.name.toUtf8());
		add(surface.material.getValue(), qsizetype(UGUID::VALUE_SIZE));
		add(surface.positions.constData(), surface.positions.size() * qsizetype(sizeof(Math::vec3)));
		add(surface.normals.constData(), surface.normals.size() * qsizetype(sizeof(Math::vec3)));
		add(surface.uv0.constData(), surface.uv0.size() * qsizetype(sizeof(Math::vec2)));
		add(surface.uv1.constData(), surface.uv1.size() * qsizetype(sizeof(Math::vec2)));
		add(surface.colors.constData(), surface.colors.size() * qsizetype(sizeof(Math::vec4)));
		add(surface.indices.constData(), surface.indices.size() * qsizetype(sizeof(int)));
		const float distances[] = {surface.min_visible, surface.max_visible, surface.min_fade, surface.max_fade};
		add(distances, qsizetype(sizeof(distances)));
		const int flags[] = {surface.viewport_mask, surface.shadow_mask, int(surface.cast_shadow),
			int(surface.cast_world_shadow), surface.lod, int(surface.shadow_lod), surface.object};
		add(flags, qsizetype(sizeof(flags)));
	}
	source.fingerprint = hash.result();
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

qint64 bentTriangles(const BendSource &source, const BendAxes &axes, int copies, double turn, float max_angle)
{
	const qint64 triangles = qint64(copies) * qint64(source.numTriangles());
	if (max_angle <= 0.0f || copies <= 0 || turn <= 0.0)
		return triangles;
	// How many triangles a plane across the copy crosses: the lengths of all of them along
	// it, in lengths of the copy.
	Math::vec3 forward, right, up;
	axis_vectors(axes, forward, right, up);
	const float length = bendBounds(source, axes).length();
	if (length < 1e-6f)
		return triangles;
	double extents = 0.0;
	for (const BendSource::Surface &surface : source.surfaces)
	{
		for (int t = 0; t + 2 < surface.indices.size(); t += 3)
		{
			float low = Math::Consts::INF;
			float high = -Math::Consts::INF;
			for (int k = 0; k < 3; ++k)
			{
				const float a = Math::dot(surface.positions[surface.indices[t + k]] * axes.scale, forward);
				low = qMin(low, a);
				high = qMax(high, a);
			}
			extents += double(high - low);
		}
	}
	const double crossed = qMin(extents / double(length), double(source.numTriangles()));
	// A cut where the curve turns by max_angle, up to twice as many: they are found by
	// halving. At most those of the deepest halving in every copy.
	const double cuts = qMin(2.0 * std::ceil(turn / double(max_angle)), double(copies) * 4095.0);
	return triangles + qint64(cuts * 2.0 * crossed);
}

void bendCopy(const BendSource &source, const BendAxes &axes, const BendBounds &bounds, const QStringList &names,
	double start, double fit, const std::function<BendFrame(double)> &frame, const Math::Vec3 &pivot, const MeshPtr &mesh,
	float max_angle)
{
	Math::vec3 forward, right, up;
	axis_vectors(axes, forward, right, up);

	// Where the curve bends or the twist turns the copy by more than max_angle between two
	// places along it, it is cut across between them, again and again: the copy gets rows
	// only where it is needed, the straight parts none.
	QVector<float> cuts;
	if (max_angle > 0.0f && bounds.length() > 1e-6f)
	{
		const float limit = std::cos(max_angle * float(Math::Consts::DEG2RAD));
		const auto turns = [&](float a, float b) {
			const BendFrame fa = frame(start + double(a) * fit);
			const BendFrame fb = frame(start + double(b) * fit);
			return Math::dot(fa.forward, fb.forward) < limit || Math::dot(fa.up, fb.up) < limit;
		};
		const std::function<void(float, float, int)> divide = [&](float a, float b, int depth) {
			// The middle too: a full turn of the twist ends looking the way it started.
			const float middle = (a + b) * 0.5f;
			if (depth >= 12 || !(turns(a, b) || turns(a, middle) || turns(middle, b)))
				return;
			divide(a, middle, depth + 1);
			cuts.append(middle);
			divide(middle, b, depth + 1);
		};
		divide(0.0f, bounds.length(), 0);
	}
	const auto along_of = [&](const Math::vec3 &p) { return Math::dot(p * axes.scale, forward) - bounds.along_min; };
	// The surface cut, made anew for every surface in the same memory: the source itself is
	// not copied.
	thread_local BendSource::Surface sliced;

	// The normals of a mesh scaled unevenly turn by the inverse of the scale.
	const Math::vec3 normal_scale(1.0f / axes.scale.x, 1.0f / axes.scale.y, 1.0f / axes.scale.z);

	for (int i = 0; i < source.surfaces.size() && i < names.size(); ++i)
	{
		if (!cuts.isEmpty())
			slice_surface(source.surfaces[i], cuts, along_of, sliced);
		const BendSource::Surface &surface = cuts.isEmpty() ? source.surfaces[i] : sliced;
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
