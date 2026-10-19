# Artist Tools for UNIGINE

Tools for everyday level work in the **UNIGINE 2.22 Editor**: place props on surfaces, build fences and roads along curves, check distances, and turn clutter into editable objects. Everything lives in one dockable window: **Windows → Artist Tools**.

[UNIGINE Store](https://store.unigine.com/en/publisher/1ee19a48-e62a-6d86-8136-9d637c69bc0c) · [Video overview](https://www.youtube.com/watch?v=lC9pwmAlltY) · [Build from source](docs/BUILDING.md)

![Fence following an editable curve in UNIGINE, with Path Placer control points visible](docs/images/path-placer.png)

## What you can do

| Tool | Use it to… |
| --- | --- |
| **Object Placer** | Place props with a click, align them to a surface, and vary their rotation and scale. |
| **Path Placer** | Arrange fences, posts, and other repeating objects along an editable curve. |
| **Road** | Make roads and paths as meshes or decals, or bend existing meshes along a curve. |
| **Measure** | Check distances, slopes, and angles, or place a person-shaped height reference. |
| **Unpack Clutter** | Convert World Clutter, Mesh Clutter, and Mesh Cluster into regular nodes you can edit individually. |

The interface supports English and Russian and follows the editor's language settings. Hover over a control for help; shortcuts are listed at the bottom of each tab.

## Install

Get the prebuilt plugin from the **Artist tool** listing on the [UNIGINE Store](https://store.unigine.com/en/publisher/1ee19a48-e62a-6d86-8136-9d637c69bc0c).

For a manual installation, copy the plugin folder into your project so the binaries end up here:

```text
<project>/bin/plugins/zloy_pingvin/artist_tool/
```

Use binaries matching your editor's platform, precision (float or double), and build (Release or Debug). Start or restart the editor, then open **Windows → Artist Tools**.

This repository contains the source code. To compile it yourself, see [Building Artist Tools](docs/BUILDING.md).

## Start using it

### Place objects on surfaces

Select a `.node`, `.mesh`, or imported model in the Asset Browser, click **Start placing**, then click a surface in the viewport. Press **Esc** to stop.

Enable **Normal Orientation** to follow the surface, or randomize rotation and scale for variation. Hold the mouse button to slide a newly placed object across the surface; use the wheel while holding it to rotate. **Shift + mouse wheel** cycles through assets of the same type in the folder.

![Props placed around a curved surface and aligned to its normals with Object Placer](docs/images/object-placer.png)

*Screenshots and the video overview are from the Store release 1.0.3; the source here also includes Road and Measure.*

### Build a fence or a row of objects

1. In **Path Placer**, click **New Path** and click surfaces to add points. Press **Esc** to finish.
2. Select assets in the Asset Browser and click **Add Selected Assets**.
3. Set their lengths and repeat counts, then click **Fill Path**.

Move the points to reshape the path. With **Live Update** enabled, the objects follow. Use corner points for sharp turns, **Closed Loop** for a loop, and **Start / End** assets for things like end posts. Paths are saved with the world, so you can return to them later.

### Make a road, path, or bent mesh

In **Road**, click **New Road**, place points, and press **Esc**. Set the width, then choose **Mesh**, **Decal**, or **Bent Mesh**. Pick a material or source mesh and click **Generate**.

Use per-point width and roll for widening sections and banked turns. Ground snapping follows the terrain; **On Curve** points let you make bridges and ramps. Generated meshes are saved to `roads/` beside the world, or to a folder you choose.

### Check scale and dimensions

In **Measure**, choose **Ruler**, **Angle**, or **Person**, then click in the viewport to place the points. You can snap to surfaces and mesh vertices, lock to an axis with **X / Y / Z**, and copy measurements as text.

### Turn clutter into editable nodes

Select clutter or cluster nodes in the World Hierarchy, choose whether to disable, keep, or delete the originals, then click **Unpack**. The resulting objects are regular nodes. **Ctrl+Z** undoes the unpacking.

## A few useful details

- Paths and roads keep their settings in the world; tool preferences are saved per project.
- Created nodes reference assets by GUID, so moving or renaming assets keeps those references intact.
- **Ctrl+Shift+F** finds the selected node's source asset in the Asset Browser. You can change this shortcut in the editor's hotkey settings under **Artist Tools**.
- When the editor's **Snap to Surface** is enabled, it handles path-point movement.

## Author and license

By **zloy_pingvin** · [Telegram](https://t.me/zloytux)

Plugin source: [MIT License](LICENSE). The UNIGINE SDK is not included and has its own license.
