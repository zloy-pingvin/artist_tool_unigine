# Artist Tools for UNIGINE

A plugin for the UNIGINE 2.22 Editor that brings tools for level artists together in a single dockable window (**Windows → Artist Tools**):

- **Object Placer** — place the selected asset on the surface under the cursor with a click in the viewport.
- **Path Placer** — arrange objects and fences along a curve that is saved with the world and can be edited later.
- **Road** — build a mesh along a curve: a road, a path or a track as a mesh or a mesh decal, or a ready mesh (a kerb, a fence, a pipe) bent along the curve.
- **Measure** — measure distances, slopes and angles in the viewport, or stand a person of a given height for scale.
- **Unpack Clutter** — convert World Clutter, Mesh Clutter, and Mesh Cluster into regular nodes.

By [zloy_pingvin](https://t.me/zloytux).

![UNIGINE](https://img.shields.io/badge/UNIGINE-2.22-blue) ![License](https://img.shields.io/badge/license-MIT-green)

## Quick Start

**Object Placer**

1. Select a mesh or node in the Asset Browser.
2. Click **Start placing**, then click in the viewport where the blue disc appears.
3. Press **Esc** or click the button again to stop placing objects.

**Path Placer**

1. Click **New Path**, then click on surfaces to add points. Press **Esc** to finish adding points.
2. Select assets in the Asset Browser and click **Add Selected Assets**. Adjust their lengths and repeat counts as needed.
3. Click **Fill Path**.
4. Move the points; the objects will follow. To return to a path later, select it in the path list or select any of its nodes.

**Road**

1. Click **New Road**, then click on surfaces to add points. Press **Esc** to finish adding points.
2. Set the width and the other settings; the grid of the future mesh is drawn over the curve.
3. Choose **Mesh**, **Decal** or **Bent Mesh**, pick a material (or the mesh to bend) in the Asset Browser, and click **Generate**.

**Measure**

1. Choose **Ruler**, **Angle** or **Person**.
2. Click in the viewport to put the points; the numbers appear in the tab.

**Unpack Clutter**

1. Select clutter nodes in the World Hierarchy.
2. Choose what to do with the source clutter and click **Unpack**.

## Features

### Object Placer

- A `.node` file is placed as a Node Reference, a `.mesh` file as a Mesh Static, and an imported model (`.fbx`) as its constituent objects with their materials. You can also place an empty Node Dummy instead of an asset.
- A disc under the cursor indicates the placement position: blue means you can click to place an object; red means placement is not allowed.
- The **Place on** filter lets you select object types (Mesh Static, Skinned, Dynamic, Cluster, Clutter, Terrain, Water, Clouds), restrict placement to immovable nodes, or use only surfaces with intersection enabled.
- Align objects with the surface normal (**Normal Orientation**), choose any of the asset's six axis directions as its **Up axis**, and set an **Offset From Surface**.
- Randomize **Local / World Offset**, **Local / World Rotation**, and **Scale** within specified minimum and maximum values.
- Hold the mouse button after clicking to move the object across surfaces. While holding it, use the mouse wheel to rotate the object around its up axis in 5° increments.
- **Shift + mouse wheel** switches to the next or previous asset of the same type in the same folder.
- Placed objects can be added under a selected parent node. Each placement is a separate undo step.

### Path Placer

- The curve passes through points represented by regular nodes, so it is saved with the world. Add points by clicking on surfaces and drag them across those surfaces.
- Each point can be **Smooth** or a **Corner**, creating a sharp bend for fence and wall corners. **Subdivide** adds a point between two selected adjacent points. Enable **Closed Loop** to close the path.
- A list of all paths in the world lets you switch between them, clone the current one, remove only its curve (the objects stay) or remove it completely; new paths can go into a chosen **Parent** node.
- Object settings include spacing (**Step**), **Forward / Up axis** selection, rotation around the vertical axis only (**Yaw Only**), shearing along slopes (**Skew Along Slope**, which keeps fence sections connected on sloping ground), and **Snap To Ground** with an offset, on the kinds of objects chosen in **Lay On** (the others are looked through), with **Recompute** after the ground has changed.
- **Live Update** keeps objects aligned with the path as you edit it. When it is disabled, click **Rearrange Now** to reposition the objects along the path.
- A **Fill pattern** can contain multiple assets, each with its own length (measured from the asset) and repeat count. Every asset is a **Pattern** one (repeated), the **Start** (placed once, first) or the **End** (placed once, right after the last repeated one), such as the posts at the ends of a fence. An asset with a length of 0 takes up no space along the path, making it suitable for a post at the joint between two sections.
- Filling is a one-time operation: the created objects are regular nodes that can be deleted, replaced, and reordered while remaining on the path. Creating more than 500 objects requires confirmation; each fill operation is limited to 5000 objects.
- Path settings and the fill pattern are stored in the path itself.

### Road

- The road is a curve of points saved with the world, like a path of the Path Placer: the same points, corners, Subdivide and Closed Loop. The settings of every road are kept in the road itself.
- **Shape**: width, strips across, step along, how smooth the bends are (**Bend Angle**), rounding of the inner edge on tight bends, and a roll for the whole road.
- Every point has its own **Roll** (banked turns; also set with the local Y ring of the manipulator), and **Width** (a multiplier of the road width); between points they change smoothly. **Side Offset** moves the whole road to the side of its curve without stretching it.
- **Ground**: lay the road on the surfaces under it with an offset, on the chosen kinds of objects only (the others are looked through), with heights cached while editing and read straight from the Landscape Terrain when possible. Single points can keep to the height of the curve (**On Curve**) for bridges and ramps.
- **Texture**: length of one repeat, repeats across, rotation.
- **Make into**: an ordinary **Mesh**, a **Decal** (DecalMesh, with lift, projection depth and visible / fade distances), or a **Bent Mesh** — a list of meshes (`.mesh`, `.node` or imported models) added like the fill pattern of the Path Placer: the Pattern ones are repeated one after another along the curve and bent to follow it, a Start and an End mesh are put once at its ends. Every mesh has its own X / Y / Z scale and a length along the curve (measured, can be corrected; it is the spacing, the mesh keeps its size); the whole set is stretched a little to fill the curve exactly. The forward and up axes are picked as in the Path Placer, the materials come with the assets, and a live preview shows the result.
- The result is cut into pieces of a maximum length (decal pieces also into at most 10,000 triangles) and saved to `roads/` next to the world or to a chosen folder. Generating again keeps the pieces and whatever was set on them by hand, including the material.
- Settings, point values and materials can be undone with **Ctrl+Z**.

### Measure

- **Ruler**: length, horizontal length, height difference, slope and the lengths along the axes. **Angle**: the angle between two sides and its supplement. **Person**: a figure of a given height for scale.
- Snap to surfaces or to mesh vertices, and to the points of other measurements; **Place On** chooses the kinds of objects the points go on, the others are looked through (a click through dense grass lands on the terrain); a magnifier for precise picking; X / Y / Z lock a measurement to an axis, Shift + X / Y / Z to a plane.
- Measurements stay in the viewport until cleared and can be copied as text.

### Unpack Clutter

- Converts World Clutter into Node References, and Mesh Clutter and Mesh Cluster into Mesh Static objects.
- Unpacks the entire World Clutter regardless of where the editor camera is looking. The tool uses its own camera during unpacking, then restores the user's camera.
- Choose what to do with the source clutter: **Disable**, **Keep enabled**, or **Delete**.
- Options include a name suffix for the generated nodes, placing the result alongside the source node, grouping instances by asset, and selecting the result.
- Asks for confirmation before creating a very large number of nodes. Unpacking can be undone with **Ctrl+Z**.

### General

- Created nodes reference assets by GUID, so renaming or moving an asset does not break the references.
- The interface is available in English and Russian. It follows the editor's language settings, including the "translate tooltips only" mode.
- Every control has a tooltip. Each tool's keyboard and mouse shortcuts are listed at the bottom of its tab.
- Tool settings are saved between sessions, for every project on its own.
- **Show Asset in Asset Browser**: a hotkey (`Ctrl+Shift+F`, changeable in the editor's hotkey settings under *Artist Tools*) and an item at the end of the context menu of nodes select in the Asset Browser the asset the selected node is made of - its `.node` or mesh, or the asset of the Node Reference it belongs to.

## Installing a Prebuilt Plugin

Copy `bin/plugins/zloy_pingvin/artist_tool/` into your project's `bin` folder, preserving the directory structure, and start the editor. Open the plugin window from the editor menu: **Windows → Artist Tools**.

## Requirements

- UNIGINE SDK **2.22** (float or double, x64), with the engine and editor headers and libraries.
- Qt **6.5.3** — the version used to build the 2.22 Editor: `msvc2019_64` on Windows, `gcc_64` on Linux.
- Windows: MSVC (Visual Studio 2022 or later), CMake 3.19+, Ninja.
- Linux: gcc 11+, CMake 3.19+, Ninja.

## Building

The plugin must be built inside a UNIGINE project because it requires the SDK's `include/` and `lib/` directories. Clone this repository into your project at:

```
<project>/source/plugins/zloy_pingvin/artist_tool/   <- this repository
```

Build **out of tree**: keep the build directory outside this folder so that build files cannot get into a package of the plugin.

Windows (from a Visual Studio x64 developer prompt):

```bat
cmake -S . -B ../../../../../artist_tool_build/win_x64 -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.5.3/msvc2019_64
cmake --build ../../../../../artist_tool_build/win_x64
```

Linux:

```sh
cmake -S . -B ~/artist_tool_build/x64 -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/opt/qt/6.5.3/gcc_64
cmake --build ~/artist_tool_build/x64
```

Options:

- `-DUNIGINE_DOUBLE=1` — link against the double-precision engine and editor libraries.
- `-DCMAKE_BUILD_TYPE=RelWithDebInfo` — produces the `d`-suffixed binary linked against the `d`-suffixed SDK libraries (the UNIGINE Store "debug" convention: optimized, with debug info).
- `-DUNIGINE_SDK_PATH=<path>` — the project or SDK root to build against, if the repository is not inside a project.
- `-DARTIST_TOOL_OUTPUT_DIR=<path>` — where to write the binary instead of the project's plugins folder.
- `-DARTIST_TOOL_DEV_PAGES=1` — adds two reference pages for plugin development to the **Debug** tab: a gallery of UI controls and a browser for the editor's built-in icons.

The binary is written to `<project>/bin/plugins/zloy_pingvin/artist_tool/`, and the editor loads it at startup. A Release editor loads only a Release plugin; a Debug editor loads only a Debug plugin. `build_linux.sh` builds all four Linux variants.

## Notes

- A path is any node with `path` and `objects` child nodes, a road any node with `path` and `mesh` child nodes. Their points, objects and pieces are regular nodes and can be edited in the World Hierarchy.
- The editor's built-in **Snap to Surface** feature and Path Placer's point dragging serve the same purpose. While **Snap to Surface** is enabled, the tool lets the editor handle point movement.
- This repository contains only the plugin source code. UNIGINE SDK files are not included; the SDK is required to build the plugin.

## License

The plugin source code is licensed under the [MIT License](LICENSE). The UNIGINE SDK is proprietary software available at [unigine.com](https://unigine.com).

## Contact

Telegram: https://t.me/zloytux
