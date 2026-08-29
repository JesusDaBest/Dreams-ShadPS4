"""Rank exact Dreams replay candidates from their raw depth coverage.

This module deliberately scores shape only.  Ordered-count invariants (counter
seeds, holes/collisions in the generated list, valid references, finite VS
outputs, and BDA faults) must be checked before a candidate is passed here.

Lower scores are better.  The score compares each candidate's D32Sfloat output
to the same captured pre-draw depth attachment, so it is independent of the
fragment color encoding and does not need a guessed texture decoder.
"""

from __future__ import annotations

import argparse
import collections
import dataclasses
import json
import math
from collections.abc import Iterable, Mapping
from pathlib import Path


Point = tuple[int, int]
BBox = tuple[int, int, int, int]


@dataclasses.dataclass(frozen=True)
class DepthMask:
    """A tightly packed one-byte foreground mask."""

    width: int
    height: int
    pixels: bytes

    def __post_init__(self) -> None:
        if self.width <= 0 or self.height <= 0:
            raise ValueError("mask dimensions must be positive")
        if len(self.pixels) != self.width * self.height:
            raise ValueError("mask payload does not match its dimensions")


@dataclasses.dataclass(frozen=True)
class DepthShapeMetrics:
    """Geometry-only measurements derived from one candidate depth mask."""

    changed_pixels: int
    component_count: int
    largest_component_pixels: int
    disconnected_pixels: int
    hole_pixels: int
    hull_area: float
    largest_component_ratio: float
    hole_ratio: float
    convex_fill_ratio: float
    cuboid_boundary_score: float
    score: float
    bbox: BBox | None

    @property
    def has_geometry(self) -> bool:
        return self.changed_pixels != 0


@dataclasses.dataclass(frozen=True)
class RankedDepthCandidate:
    name: str
    metrics: DepthShapeMetrics


def depth_change_mask(
    pre_depth: bytes | bytearray | memoryview,
    replay_depth: bytes | bytearray | memoryview,
    width: int,
    height: int,
    *,
    row_length: int | None = None,
) -> DepthMask:
    """Return pixels whose raw D32Sfloat bits changed during the draw.

    Raw comparison is intentional.  It preserves NaN payloads and signed zero
    exactly as Vulkan readback produced them.  ``row_length`` is expressed in
    pixels and permits a padded buffer-image copy.
    """

    if width <= 0 or height <= 0:
        raise ValueError("depth dimensions must be positive")
    pitch = width if row_length is None else row_length
    if pitch < width:
        raise ValueError("row_length cannot be smaller than width")
    expected = pitch * height * 4
    before = memoryview(pre_depth).cast("B")
    after = memoryview(replay_depth).cast("B")
    if len(before) != expected or len(after) != expected:
        raise ValueError(
            f"depth payloads must each contain exactly {expected} bytes "
            f"for {width}x{height} with row_length={pitch}"
        )

    output = bytearray(width * height)
    for y in range(height):
        source_row = y * pitch * 4
        target_row = y * width
        for x in range(width):
            source = source_row + x * 4
            output[target_row + x] = before[source : source + 4] != after[source : source + 4]
    return DepthMask(width, height, bytes(output))


def _foreground_components(mask: DepthMask) -> tuple[list[int], int]:
    """Return the largest 8-connected component and the component count."""

    width, height = mask.width, mask.height
    pixels = mask.pixels
    visited = bytearray(len(pixels))
    largest: list[int] = []
    component_count = 0
    neighbors = (
        (-1, -1),
        (0, -1),
        (1, -1),
        (-1, 0),
        (1, 0),
        (-1, 1),
        (0, 1),
        (1, 1),
    )

    for origin, foreground in enumerate(pixels):
        if not foreground or visited[origin]:
            continue
        component_count += 1
        visited[origin] = 1
        queue: collections.deque[int] = collections.deque((origin,))
        component: list[int] = []
        while queue:
            index = queue.popleft()
            component.append(index)
            y, x = divmod(index, width)
            for dx, dy in neighbors:
                nx, ny = x + dx, y + dy
                if nx < 0 or nx >= width or ny < 0 or ny >= height:
                    continue
                neighbor = ny * width + nx
                if pixels[neighbor] and not visited[neighbor]:
                    visited[neighbor] = 1
                    queue.append(neighbor)
        if len(component) > len(largest):
            largest = component
    return largest, component_count


def _component_mask(width: int, height: int, component: Iterable[int]) -> bytearray:
    result = bytearray(width * height)
    for index in component:
        result[index] = 1
    return result


def _bbox(width: int, component: Iterable[int]) -> BBox:
    iterator = iter(component)
    first = next(iterator)
    first_y, first_x = divmod(first, width)
    min_x = max_x = first_x
    min_y = max_y = first_y
    for index in iterator:
        y, x = divmod(index, width)
        min_x = min(min_x, x)
        max_x = max(max_x, x)
        min_y = min(min_y, y)
        max_y = max(max_y, y)
    return min_x, min_y, max_x, max_y


def _hole_pixels(component_mask: bytearray, width: int, bbox: BBox) -> int:
    """Count 4-connected background pockets enclosed by an 8-connected shape."""

    min_x, min_y, max_x, max_y = bbox
    box_width = max_x - min_x + 1
    box_height = max_y - min_y + 1
    exterior = bytearray(box_width * box_height)
    queue: collections.deque[tuple[int, int]] = collections.deque()

    def enqueue(x: int, y: int) -> None:
        local = (y - min_y) * box_width + (x - min_x)
        if exterior[local] or component_mask[y * width + x]:
            return
        exterior[local] = 1
        queue.append((x, y))

    for x in range(min_x, max_x + 1):
        enqueue(x, min_y)
        enqueue(x, max_y)
    for y in range(min_y, max_y + 1):
        enqueue(min_x, y)
        enqueue(max_x, y)

    while queue:
        x, y = queue.popleft()
        for nx, ny in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
            if nx < min_x or nx > max_x or ny < min_y or ny > max_y:
                continue
            enqueue(nx, ny)

    holes = 0
    for y in range(min_y, max_y + 1):
        local_row = (y - min_y) * box_width
        image_row = y * width
        for x in range(min_x, max_x + 1):
            local = local_row + (x - min_x)
            if not component_mask[image_row + x] and not exterior[local]:
                holes += 1
    return holes


def _boundary_pixel_corners(component_mask: bytearray, width: int, height: int) -> list[Point]:
    """Return doubled half-pixel corners for foreground boundary pixels."""

    corners: set[Point] = set()
    for index, foreground in enumerate(component_mask):
        if not foreground:
            continue
        y, x = divmod(index, width)
        boundary = (
            x == 0
            or x + 1 == width
            or y == 0
            or y + 1 == height
            or not component_mask[index - 1]
            or not component_mask[index + 1]
            or not component_mask[index - width]
            or not component_mask[index + width]
        )
        if boundary:
            corners.update(
                (
                    (2 * x - 1, 2 * y - 1),
                    (2 * x + 1, 2 * y - 1),
                    (2 * x + 1, 2 * y + 1),
                    (2 * x - 1, 2 * y + 1),
                )
            )
    return list(corners)


def _cross(origin: Point, a: Point, b: Point) -> int:
    return (a[0] - origin[0]) * (b[1] - origin[1]) - (a[1] - origin[1]) * (
        b[0] - origin[0]
    )


def _convex_hull(points: Iterable[Point]) -> list[Point]:
    unique = sorted(set(points))
    if len(unique) <= 1:
        return unique
    lower: list[Point] = []
    for point in unique:
        while len(lower) >= 2 and _cross(lower[-2], lower[-1], point) <= 0:
            lower.pop()
        lower.append(point)
    upper: list[Point] = []
    for point in reversed(unique):
        while len(upper) >= 2 and _cross(upper[-2], upper[-1], point) <= 0:
            upper.pop()
        upper.append(point)
    return lower[:-1] + upper[:-1]


def _polygon_twice_area(polygon: list[Point]) -> int:
    if len(polygon) < 3:
        return 0
    return abs(
        sum(
            point[0] * polygon[(index + 1) % len(polygon)][1]
            - point[1] * polygon[(index + 1) % len(polygon)][0]
            for index, point in enumerate(polygon)
        )
    )


def _simplify_convex_polygon(polygon: list[Point], target_vertices: int) -> list[Point]:
    """Remove the least area-significant corners until ``target_vertices`` remain."""

    result = list(polygon)
    while len(result) > target_vertices:
        removal = min(
            range(len(result)),
            key=lambda index: (
                abs(
                    _cross(
                        result[(index - 1) % len(result)],
                        result[index],
                        result[(index + 1) % len(result)],
                    )
                ),
                index,
            ),
        )
        del result[removal]
    return result


def _parallel_error(polygon: list[Point]) -> float:
    if len(polygon) not in (4, 6):
        return 1.0
    half = len(polygon) // 2
    errors: list[float] = []
    for index in range(half):
        a0 = polygon[index]
        a1 = polygon[(index + 1) % len(polygon)]
        b0 = polygon[(index + half) % len(polygon)]
        b1 = polygon[(index + half + 1) % len(polygon)]
        ax, ay = a1[0] - a0[0], a1[1] - a0[1]
        bx, by = b1[0] - b0[0], b1[1] - b0[1]
        denominator = math.hypot(ax, ay) * math.hypot(bx, by)
        errors.append(1.0 if denominator == 0 else abs(ax * by - ay * bx) / denominator)
    return sum(errors) / len(errors)


def _symmetry_error(polygon: list[Point]) -> float:
    if len(polygon) not in (4, 6):
        return 1.0
    half = len(polygon) // 2
    center_x = sum(point[0] for point in polygon) / len(polygon)
    center_y = sum(point[1] for point in polygon) / len(polygon)
    xs = [point[0] for point in polygon]
    ys = [point[1] for point in polygon]
    diagonal = math.hypot(max(xs) - min(xs), max(ys) - min(ys))
    if diagonal == 0:
        return 1.0
    errors = []
    for index in range(half):
        opposite = polygon[index + half]
        midpoint_x = (polygon[index][0] + opposite[0]) * 0.5
        midpoint_y = (polygon[index][1] + opposite[1]) * 0.5
        errors.append(math.hypot(midpoint_x - center_x, midpoint_y - center_y) / diagonal)
    return min(1.0, (sum(errors) / len(errors)) * 4.0)


def _cuboid_boundary_score(hull: list[Point]) -> float:
    """Measure how well the convex outline fits a four- or six-edge box projection."""

    if len(hull) < 4:
        return 1.0
    original_area = _polygon_twice_area(hull)
    if original_area == 0:
        return 1.0
    fits: list[float] = []
    for target in (4, 6):
        if len(hull) < target:
            continue
        simplified = _simplify_convex_polygon(hull, target)
        retained_area = _polygon_twice_area(simplified)
        area_loss = max(0.0, 1.0 - retained_area / original_area)
        # Straight four/six-sided coverage is the strongest signal.  Parallelism and central
        # symmetry are deliberately lower-weight because a perspective cube is not exactly affine.
        fit = (
            area_loss * 0.70
            + _parallel_error(simplified) * 0.20
            + _symmetry_error(simplified) * 0.10
        )
        fits.append(min(1.0, fit))
    return min(fits, default=1.0)


def score_depth_mask(mask: DepthMask) -> DepthShapeMetrics:
    """Compute connectedness, closure, convex fill, and box-outline quality."""

    changed = sum(mask.pixels)
    if changed == 0:
        return DepthShapeMetrics(
            changed_pixels=0,
            component_count=0,
            largest_component_pixels=0,
            disconnected_pixels=0,
            hole_pixels=0,
            hull_area=0.0,
            largest_component_ratio=0.0,
            hole_ratio=1.0,
            convex_fill_ratio=0.0,
            cuboid_boundary_score=1.0,
            score=math.inf,
            bbox=None,
        )

    largest, component_count = _foreground_components(mask)
    largest_count = len(largest)
    disconnected = changed - largest_count
    bbox = _bbox(mask.width, largest)
    largest_mask = _component_mask(mask.width, mask.height, largest)
    holes = _hole_pixels(largest_mask, mask.width, bbox)
    hull = _convex_hull(
        _boundary_pixel_corners(largest_mask, mask.width, mask.height)
    )
    # Hull coordinates are doubled half-pixel coordinates.  Shoelace gives twice the area in
    # doubled units, so divide by eight to recover pixel-square area.
    hull_area = _polygon_twice_area(hull) / 8.0
    largest_ratio = largest_count / changed
    hole_ratio = min(1.0, holes / max(hull_area, 1.0))
    convex_fill = min(1.0, largest_count / max(hull_area, 1.0))
    cuboid_score = _cuboid_boundary_score(hull)
    disconnected_ratio = disconnected / changed
    score = (
        disconnected_ratio * 4.0
        + hole_ratio * 4.0
        + (1.0 - convex_fill) * 3.0
        + cuboid_score * 2.0
    )
    return DepthShapeMetrics(
        changed_pixels=changed,
        component_count=component_count,
        largest_component_pixels=largest_count,
        disconnected_pixels=disconnected,
        hole_pixels=holes,
        hull_area=hull_area,
        largest_component_ratio=largest_ratio,
        hole_ratio=hole_ratio,
        convex_fill_ratio=convex_fill,
        cuboid_boundary_score=cuboid_score,
        score=score,
        bbox=bbox,
    )


def score_depth_candidate(
    pre_depth: bytes | bytearray | memoryview,
    replay_depth: bytes | bytearray | memoryview,
    width: int,
    height: int,
    *,
    row_length: int | None = None,
) -> DepthShapeMetrics:
    return score_depth_mask(
        depth_change_mask(pre_depth, replay_depth, width, height, row_length=row_length)
    )


def rank_depth_candidates(
    pre_depth: bytes | bytearray | memoryview,
    candidates: Mapping[str, bytes] | Iterable[tuple[str, bytes]],
    width: int,
    height: int,
    *,
    row_length: int | None = None,
) -> tuple[RankedDepthCandidate, ...]:
    """Score candidates and return a deterministic best-first ordering."""

    items = candidates.items() if isinstance(candidates, Mapping) else candidates
    results: list[RankedDepthCandidate] = []
    names: set[str] = set()
    for name, depth in items:
        if not name:
            raise ValueError("candidate names must be non-empty")
        if name in names:
            raise ValueError(f"duplicate candidate name: {name}")
        names.add(name)
        results.append(
            RankedDepthCandidate(
                name,
                score_depth_candidate(
                    pre_depth, depth, width, height, row_length=row_length
                ),
            )
        )
    results.sort(
        key=lambda result: (
            not result.metrics.has_geometry,
            result.metrics.score,
            -result.metrics.convex_fill_ratio,
            -result.metrics.changed_pixels,
            result.name,
        )
    )
    return tuple(results)


def _read_manifest_dimensions(path: Path) -> tuple[int, int, int]:
    values: dict[str, str] = {}
    for line_number, raw_line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw_line.rstrip("\r\n")
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        key, separator, value = line.partition("\t")
        if not separator or not key:
            raise ValueError(f"{path}:{line_number}: expected key<TAB>value")
        values[key] = value

    try:
        width = int(values["pre_depth_width"], 0)
        height = int(values["pre_depth_height"], 0)
        row_length = int(values["pre_depth_row_length"], 0)
        post_width = int(values["post_depth_width"], 0)
        post_height = int(values["post_depth_height"], 0)
        post_row_length = int(values["post_depth_row_length"], 0)
        pre_layers = int(values.get("pre_depth_layers", "1"), 0)
        post_layers = int(values.get("post_depth_layers", "1"), 0)
    except (KeyError, ValueError) as exc:
        raise ValueError(f"{path}: missing or invalid depth dimensions") from exc

    if (width, height, row_length) != (post_width, post_height, post_row_length):
        raise ValueError(f"{path}: pre/post depth dimensions differ")
    if pre_layers != 1 or post_layers != 1:
        raise ValueError(f"{path}: only a single depth layer is supported")
    return width, height, row_length


def _shared_candidate_input(
    root: Path, candidate_paths: list[Path], filename: str
) -> list[Path]:
    direct = root / filename
    if direct.is_file():
        return [direct]
    paths = sorted({candidate.parent / filename for candidate in candidate_paths})
    missing = [path for path in paths if not path.is_file()]
    if missing:
        raise ValueError(f"missing {filename} beside candidate output: {missing[0]}")
    return paths


def _candidate_name(root: Path, path: Path) -> str:
    relative = path.relative_to(root)
    if path.name == "replay-depth.bin":
        parent = relative.parent.as_posix()
        return root.name if parent == "." else parent
    return relative.as_posix()


def rank_depth_output_directory(
    output_directory: str | Path,
    *,
    candidate_glob: str = "**/replay-depth.bin",
    pre_depth_path: str | Path | None = None,
    manifest_path: str | Path | None = None,
    width: int | None = None,
    height: int | None = None,
    row_length: int | None = None,
) -> tuple[RankedDepthCandidate, ...]:
    """Rank raw depth outputs stored under one candidate-run directory.

    The default layout is either a shared ``pre-depth.bin``/``manifest.tsv``
    at the directory root, or a complete replay bundle per candidate::

        output-directory/
          candidate-name/{manifest.tsv,pre-depth.bin,replay-depth.bin}

    ``candidate_glob`` can select a flat naming scheme instead.  If inputs are
    duplicated beside candidates, every copy must be byte-identical and every
    manifest must declare identical depth dimensions.  This prevents ranking
    candidates against different starting scenes by accident.
    """

    root = Path(output_directory).resolve()
    if not root.is_dir():
        raise ValueError(f"candidate output directory does not exist: {root}")
    candidate_paths = sorted(path for path in root.glob(candidate_glob) if path.is_file())
    if not candidate_paths:
        raise ValueError(f"no candidate depth outputs match {candidate_glob!r} under {root}")

    if pre_depth_path is not None:
        pre_paths = [Path(pre_depth_path).resolve()]
    else:
        pre_paths = _shared_candidate_input(root, candidate_paths, "pre-depth.bin")
    try:
        pre_depth = pre_paths[0].read_bytes()
    except OSError as exc:
        raise ValueError(f"cannot read pre-depth input {pre_paths[0]}: {exc}") from exc
    for path in pre_paths[1:]:
        if path.read_bytes() != pre_depth:
            raise ValueError(f"candidate pre-depth inputs differ: {pre_paths[0]} and {path}")

    explicit_dimensions = width is not None or height is not None or row_length is not None
    if explicit_dimensions:
        if width is None or height is None:
            raise ValueError("width and height must be provided together")
        effective_row_length = width if row_length is None else row_length
    else:
        if manifest_path is not None:
            manifests = [Path(manifest_path).resolve()]
        else:
            manifests = _shared_candidate_input(root, candidate_paths, "manifest.tsv")
        dimensions = _read_manifest_dimensions(manifests[0])
        for path in manifests[1:]:
            if _read_manifest_dimensions(path) != dimensions:
                raise ValueError(f"candidate depth dimensions differ: {manifests[0]} and {path}")
        width, height, effective_row_length = dimensions

    assert width is not None and height is not None
    candidates: list[tuple[str, bytes]] = []
    for path in candidate_paths:
        try:
            depth = path.read_bytes()
        except OSError as exc:
            raise ValueError(f"cannot read candidate depth output {path}: {exc}") from exc
        candidates.append((_candidate_name(root, path), depth))
    return rank_depth_candidates(
        pre_depth,
        candidates,
        width,
        height,
        row_length=effective_row_length,
    )


def _metrics_json(candidate: RankedDepthCandidate) -> dict[str, object]:
    metrics = dataclasses.asdict(candidate.metrics)
    if not math.isfinite(candidate.metrics.score):
        metrics["score"] = None
    return {"name": candidate.name, **metrics}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Rank exact Dreams replay candidates by depth-mask shape completeness."
    )
    parser.add_argument("output_directory", type=Path)
    parser.add_argument("--glob", default="**/replay-depth.bin", dest="candidate_glob")
    parser.add_argument("--pre-depth", type=Path)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--width", type=int)
    parser.add_argument("--height", type=int)
    parser.add_argument("--row-length", type=int)
    parser.add_argument("--limit", type=int, default=20)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    if args.limit < 0:
        parser.error("--limit must be non-negative")

    try:
        ranked = rank_depth_output_directory(
            args.output_directory,
            candidate_glob=args.candidate_glob,
            pre_depth_path=args.pre_depth,
            manifest_path=args.manifest,
            width=args.width,
            height=args.height,
            row_length=args.row_length,
        )
    except ValueError as exc:
        parser.error(str(exc))
    shown = ranked[: args.limit] if args.limit else ranked
    if args.json:
        print(json.dumps([_metrics_json(candidate) for candidate in shown], indent=2))
        return 0

    print(
        "rank score      pixels comps disconnected holes fill     boundary candidate"
    )
    for rank, candidate in enumerate(shown, 1):
        metrics = candidate.metrics
        score = "empty" if not metrics.has_geometry else f"{metrics.score:.6f}"
        print(
            f"{rank:4d} {score:>10} {metrics.changed_pixels:7d} "
            f"{metrics.component_count:5d} {metrics.disconnected_pixels:12d} "
            f"{metrics.hole_pixels:5d} {metrics.convex_fill_ratio:8.5f} "
            f"{metrics.cuboid_boundary_score:8.5f} {candidate.name}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
