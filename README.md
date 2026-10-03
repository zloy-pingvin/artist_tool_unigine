# Artist Tools for UNIGINE

A plugin for the UNIGINE 2.22 Editor that brings tools for level artists together in a single dockable window (**Windows → Artist Tools**):

- **Object Placer** — place the selected asset on the surface under the cursor with a click in the viewport.
- **Path Placer** — arrange objects and fences along a curve that is saved with the world and can be edited later.
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
- A list of all paths in the world lets you switch between them.
- Object settings include spacing (**Step**), **Forward / Up axis** selection, rotation around the vertical axis only (**Yaw Only**), shearing along slopes (**Skew Along Slope**, which keeps fence sections connected on sloping ground), and **Snap To Ground** with an offset.
- **Live Update** keeps objects aligned with the path as you edit it. When it is disabled, click **Rearrange Now** to reposition the objects along the path.
- A **Fill pattern** can contain multiple assets, each with its own length (measured from the asset) and repeat count. An optional **Tail** asset is always placed last, such as a post at the end of a fence. An asset with a length of 0 takes up no space along the path, making it suitable for a post at the joint between two sections.
- Filling is a one-time operation: the created objects are regular nodes that can be deleted, replaced, and reordered while remaining on the path. Creating more than 500 objects requires confirmation; each fill operation is limited to 5000 objects.
- Path settings and the fill pattern are stored in the path itself.

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
- Tool settings are saved between sessions.

## Installing a Prebuilt Plugin

Copy `bin/plugins/zloy_pingvin/artist_tool/` into your project's `bin` folder, preserving the directory structure, and start the editor. Open the plugin window from the editor menu: **Windows → Artist Tools**.

## Requirements

- UNIGINE SDK **2.22**, double precision, Windows x64 — the configuration used to build and run the plugin.
- Qt **6.5.3** `msvc2019_64` — the version used to build the 2.22 Editor. The project file locates it through the `UNIGINE_QT653ROOT` environment variable.
- Visual Studio 2022 or later with the MSVC **v143** toolset and a Windows 10/11 SDK.

## Building

The plugin must be built inside a UNIGINE project because it requires the SDK's `include/` and `lib/` directories. Clone this repository into your project at:

```
<project>/source/plugins/zloy_pingvin/artist_tool/   <- this repository
```

Build `artist_tool.vcxproj` with MSBuild. The project file generated by the SDK specifies an older toolset that is incompatible with Qt 6.5, so specify the toolset and Windows SDK version on the command line:

```bat
MSBuild artist_tool.vcxproj /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143 /p:WindowsTargetPlatformVersion=<your Windows SDK version>
```

Replace `<your Windows SDK version>` with your installed Windows SDK version.

The library is written to `<project>/bin/plugins/zloy_pingvin/artist_tool/`, and the editor loads it at startup. A Release editor loads only a Release plugin; a Debug editor loads only a Debug plugin.

## Notes

- A path is any node with `path` and `objects` child nodes. Its points and objects are regular nodes and can be edited in the World Hierarchy.
- The editor's built-in **Snap to Surface** feature and Path Placer's point dragging serve the same purpose. While **Snap to Surface** is enabled, the tool lets the editor handle point movement.
- This repository contains only the plugin source code. UNIGINE SDK files are not included; the SDK is required to build the plugin.

## License

The plugin source code is licensed under the [MIT License](LICENSE). The UNIGINE SDK is proprietary software available at [unigine.com](https://unigine.com).

## Contact

Telegram: https://t.me/zloytux
