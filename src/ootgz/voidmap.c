/* Where Link voids out (FINDINGS §9.8).
 *
 * The void check doesn't look at the collision. It looks at the ground map: space cut into chunks of 256^3
 * one-unit cells, each chunk a sparse octree of 8 levels, a cell set where there's walkable space just over
 * the ground. Each frame the game casts down from Link into it (probe A) and keeps the nearest cell it has
 * found round Link (probe C). Link voids when
 *   - nothing is under Link and that nearest cell is 8 or more away level (player flags +0x13D0 bit 25), or
 *   - the cell under Link is a deadly one: its record's field 20 is set and field 21 is over 3 (bit 19).
 * Other voids aren't in the map: the areas placed to void Link (airwall.c draws those, from data.c; the holes
 * down into grottos are among them) and floors tagged Abyss (not drawn).
 *
 * How probe C (the 8-unit fence's anchor) moves (main+0x38AEF0, 1.0.0 main+0x4D8810):
 *   Each frame, after probe A casts down, probe C computes the 3D distance from Link's query position Q to its
 *   remembered point C, then shoots 64 Fibonacci-sphere rays (6-7 per frame across a 10-frame cycle) from Q
 *   through the VolumeStats octree. A ray hits when it exits a mapped leaf cell across a face whose 6-bit
 *   Intersect (field 0) or IntersectWater (field 19) bit is set. If any ray hits at a strictly smaller 3D
 *   distance than |Q - C|, C shifts to that hit; otherwise C stays frozen.
 *
 * Drawn: the edges of the mapped ground (past them probe A has nothing to hit); the deadly cells as columns,
 * from the cell up to the next mapped one over it (anywhere in one the cast lands on the cell); a fence of 8
 * round the point probe C holds now, which is where Link voids while off the map; and translucent green/red
 * shading on the ground around Link showing where probe C's 64-ray check will shift the fence (green) or fail
 * to move it (red). The cells round Link are read out of the octrees every so often, not every frame. */
#include "voidmap.h"

#include "lang.h"
#include "link.h"
#include "sys.h"
#include "z64hd.h"

float sinf(float x); /* from sdk */
float cosf(float x);
float sqrtf(float x);

bool voidmap_shown;
bool voidmap_ground;
bool voidmap_deadly = true, voidmap_ring = true, voidmap_shift = true;
bool voidmap_areas = true, voidmap_bounds = true;
bool voidmap_xray;

/* the ground map: [[[[main + voxel_mgr_ptr]] + this] + this] is its manager */
#define MGR_OWNER 0xC0
#define MGR_IN_OWNER 0x170
#define MGR_LIST_HEAD 0x10  /* a linked list of the loaded areas */
#define MGR_LIST_FIRST 0x18
#define MGR_NODE_OFFSET 0x24 /* i32: where the list's node is in an area */
#define NODE_NEXT 0x08
#define AREA_HOLDER 0x90
#define HOLDER_COUNT 0x28   /* i32 */
#define HOLDER_CHUNKS 0x30  /* pointers to the chunks */
#define MAX_AREAS 4096      /* only so a damaged list can't loop for ever */
#define MAX_CHUNKS 4096     /* in all the areas together, for the same reason */
/* a chunk: its origin (3 x i32) at +0, then per level a record block and the nodes */
#define CHUNK_RECORDS 0x28  /* + level * RECORDS_SIZE: +0x10 data, +0x18 bits a record, +0x1C format */
#define RECORDS_SIZE 0x28
#define RECORDS_DATA 0x10
#define RECORDS_BITS 0x18
#define RECORDS_FORMAT 0x1C
#define CHUNK_NODES 0x168   /* + level * 8: that level's node words */
#define LEVELS 8
#define CHUNK_CELLS 256
#define CHUNK_MARGIN 3      /* chunks overlap: one only answers for cells 3..253 of its own */
#define NODE_CHILDREN 8
#define NODE_CHILD_MASK 0xFFu /* a node word: which of its 8 children exist, and where the first one is */
#define NODE_FIRST_SHIFT 8
/* the record formats (main + voxel_formats): which fields a record has and where */
#define FORMAT_SIZE 0x74
#define FORMAT_COUNT 7      /* a format past these reads as format 0, as the game does */
#define FORMAT_PRESENT 0x04 /* + field: u8, 1 if the format has it */
#define FORMAT_BIT 0x1C     /* + field * 4: i32, its first bit in a record */
#define FIELD_INTERSECT 0       /* 6 bits: -X, +X, -Y, +Y, -Z, +Z faces touching solid ground */
#define FIELD_INTERSECT_BITS 6
#define FIELD_INTERSECT_WATER 19 /* 6 bits: same for water surfaces (ORed with field 0 in probe raycasts) */
#define FIELD_DEADLY 20
#define FIELD_DEADLY_BITS 1
#define FIELD_DEPTH 21      /* times 2/3 */
#define FIELD_DEPTH_BITS 4
#define DEADLY_OVER 4       /* field 21 over 3.0 is 5 or more of its steps */

/* packed per-cell flags in cells[]: bits 0..5 are (Intersect | IntersectWater) face mask */
#define CELL_FACE_MASK 0x3Fu
#define CELL_MAPPED 0x40u
#define CELL_DEADLY 0x80u

/* Link's: [[actor + 0x270] + ...] */
#define ACTOR_PARTS 0x270
#define PARTS_PLAYER 0x358
#define PARTS_PROBE 0x460
#define ACTOR_POS 0x31C         /* Link's feet */
#define PLAYER_GROUND_FLAGS 0x13D0
#define FLAG_DEADLY_CELL 0x80000u
#define FLAG_FAR_FROM_GROUND 0x2000000u
#define PROBE_OWNER 0xA0        /* [probe + 0xA0] -> +0x150: config with max ray length at +0x44 */
#define PROBE_OWNER_CFG 0x150
#define PROBE_CFG_RAY_LEN 0x44  /* f32; < 0 uses 5000.0 */
#define PROBE_CFG_READY 0x65    /* u8 bit 0: direct config ready */
#define PROBE_FLAGS 0xA8        /* u32 */
#define PROBE_OUTSIDE 0x40000u  /* Link is more than 5 outside the probe's own box: sets the deadly flag too */
#define PROBE_POS 0xE0          /* 3 x f32: live probe query origin */
#define PROBE_AABB_MAX 0xEC     /* 3 x f32: VolumeStats AABB max (X/Z checked +5m; Y not checked) */
#define PROBE_AABB_MIN 0xF8     /* 3 x f32: VolumeStats AABB min (X/Y/Z checked -5m) */
#define PROBE_BOX_MARGIN 5.0f   /* main+0x38CE24: > 5 units past AABB min(X/Y/Z) or max(X/Z) sets PROBE_OUTSIDE */
#define PROBE_A_POINT 0x360     /* where the cast down landed */
#define PROBE_A_HIT 0x37C       /* u8 bit 0: mapped ground under Link */
#define PROBE_C_POINT 0xBA0     /* the nearest cell found */
#define PROBE_C_HIT 0xBBC       /* u8 bit 0 */
#define PROBE_C_LEVEL 0x115C    /* f32: how far it is, level */
#define PROBE_HEIGHT 0x1164     /* f32: multiplied by 0.6 for probe A's cast start height */
#define PROBE_DEFAULT_RAY 5000.0f
#define PROBE_DEFAULT_DY 0.585f /* Link's physics centre above his feet, fallback */
#define PROBE_RAYS 64
#define GOLDEN_ANGLE 3.8832221f /* 2*pi / phi (0x407886b6 in main) */
#define VOID_DISTANCE 8.0f

/* the cells kept: a box round Link. The cast down has no end to speak of (5000), so a deadly cell voids Link
 * from far overhead: the box reaches a long way under Link */
#define BOX_X 48
#define BOX_Y 64
#define BOX_Z 48
#define BOX_BELOW 48 /* of BOX_Y */
#define BOX_CELLS (BOX_X * BOX_Y * BOX_Z)
#define REBUILD_MOVED 4     /* cells Link has moved from the box's middle */
#define REBUILD_FRAMES 120  /* and this often anyway: chunks load as Link goes */
#define RETRY_FRAMES 30     /* while no chunk is loaded, how often to look again */
#define MAX_EDGES 6000      /* before the nearest VOIDMAP_MAX_LINES of them are kept */
#define MAX_WALLS 2000      /* the columns' sides, before the nearest VOIDMAP_MAX_FACES are kept */
#define LIFT 0.12f          /* over the cell's floor, so the ground doesn't cut the lines */
#define POST_SLOTS 8192     /* a rebuild's upright lines, each once: a hash table, a power of two */
#define POST_SLOT_BITS 13
#define MAX_POSTS (POST_SLOTS / 2)
#define HASH_MULTIPLIER 2654435761u /* Knuth's, 2^32 / the golden ratio */
#define EDGE_STEPS 32       /* the edges are sorted by distance into this many steps */
#define HEIGHT_COUNTS 0.25f /* of an edge's height off Link's in that distance: a column's foot far under Link
                               matters as much as what's beside Link */
#define WALL_RANGE 24.0f    /* the columns' sides farther than this aren't filled in */
#define WALL_RINGS 4        /* they're taken a ring of distance at a time, the nearest first */
#define RING_SEGMENTS 32
#define RING_POST_EVERY 4   /* an upright at every 4th segment: eight in all */
#define RING_REACH 2.0f     /* the fence, this far over and under Link's feet */
#define DEADLY_PAD 0.6f     /* Probe A casts a 3x3 grid at +/-0.6 in X/Z when centre ray misses (main+0x38AF50) */
#define BOUNDS_SHOW_RANGE 28.0f /* draw any side/floor of the map's outer AABB within this distance of Link */
#define BOUNDS_PATCH_HALF 12.0f /* half-width of the drawn wall/floor patch along the boundary plane */
#define BOUNDS_WALL_HALF 4.0f   /* vertical half-height of drawn outer-box side walls around Link */
#define TWO_PI 6.2831853f

/* Ground shift assessment grid around Link: 1x1 world-snapped tiles within SHIFT_RADIUS_F horizontally. */
#define SHIFT_RADIUS 11
#define SHIFT_GRID (2 * SHIFT_RADIUS + 1)
#define SHIFT_RADIUS_SQ (11.5f * 11.5f)
#define SHIFT_GROUND_LIFT 0.06f /* just over the collision surface so it stays crisp over slopes */
#define SHIFT_WALK_NY 0.35f     /* minimum normal.y to count as walkable ground (skips sheer walls) */
#define SHIFT_FLAT_NY 0.96f     /* normal.y above which neighbouring tiles merge into one flat quad */
#define SHIFT_MERGE_DY 0.08f    /* max height step to merge adjacent flat tiles of the same shade */
#define SHIFT_MAX_SLOPE 2.5f    /* clamp -nx/ny and -nz/ny on steep slopes */
#define SHIFT_RINGS 4           /* distance rings for nearest-first face emission */
#define PHIVE_LAYER_GROUND 0x1A /* player ground collision layer (main+0xA46364 / 0x27CB4F8) */

enum { SHIFT_NONE = 0, SHIFT_FAIL = 1, SHIFT_OK = 2 };

typedef struct { float a[3], b[3]; uint8_t colour; } edge_t;

typedef struct {
  float y, nx, ny, nz;
  uint8_t sampled; /* 1: ground height sampled; 0: needs raycast */
  uint8_t has_ground;
  uint8_t state;   /* SHIFT_NONE, SHIFT_FAIL, SHIFT_OK */
  uint8_t eval_ok; /* 1: Probe C check up to date for current box and anchor */
} shift_cell_t;

static const int box_size[3] = { BOX_X, BOX_Y, BOX_Z };
static const int box_middle[3] = { BOX_X / 2, BOX_BELOW, BOX_Z / 2 }; /* where Link's cell is in it */

static uint8_t cells[BOX_CELLS];
static bool layer_mapped[BOX_Y], layer_deadly[BOX_Y]; /* whether a layer has any such cell */
static int16_t below_y[BOX_Z][BOX_X];     /* highest mapped cell below box_min[1], matching Probe A's 5000m cast */
static uint8_t below_flags[BOX_Z][BOX_X]; /* CELL_MAPPED / CELL_DEADLY of that deepest-visible cell */
static int box_min[3];
static bool box_valid;
static edge_t edges[MAX_EDGES];
static uint8_t edge_step[MAX_EDGES]; /* nearest_edges' */
static int n_edges;
static aw_face_t walls[MAX_WALLS];
static int n_walls;
static uint32_t posts[POST_SLOTS]; /* the uprights made this rebuild; 0: free */
static int n_posts;
static bool full_logged;           /* the log has said the edges or walls ran out, for this map's stretch */
static struct { bool on; uint8_t colour; float point[3], y; } fence; /* this frame's (ring) */
static aw_face_t bounds_faces[5];  /* this frame's nearby outer-box walls (up to 4) + minY-5 floor */
static int n_bounds_faces;
static uint32_t frames, built_frame;
static bool built_ground, built_deadly;
static uint32_t void_flags_were;   /* voidmap_frame's: the void flags last frame */

/* Probe C's 64 Fibonacci-sphere ray directions, built once */
static float probe_rays[PROBE_RAYS][3];
static bool probe_rays_ready;

/* Cached ground heightmap and shift evaluation around Link */
static shift_cell_t shift_grid[SHIFT_GRID][SHIFT_GRID];
static shift_cell_t shift_tmp[SHIFT_GRID][SHIFT_GRID];
static bool shift_grid_valid;
static int shift_ox, shift_oz; /* world integer cell at shift_grid[SHIFT_RADIUS][SHIFT_RADIUS] */
static float shift_ref_y;      /* Link's feet y when the heightmap was built */
static uint32_t shift_height_frame;
static bool shift_anchor_had;
static float shift_anchor[3];  /* Probe C anchor when shift_grid was last evaluated */
static float shift_eval_dy;    /* probe vertical offset used for last evaluation */
static aw_face_t shift_quads[SHIFT_GRID * SHIFT_GRID];
static int n_shift_quads;
static bool shift_quads_dirty;

static inline float absf(float x)
{
  return x < 0 ? -x : x;
}

/* Single AArch64 hardware fsqrt without GCC's errno fallback call to libm sqrtf. */
static inline float fsqrtf(float x)
{
#ifdef __aarch64__
  float r;
  __asm__("fsqrt %s0, %s1" : "=w"(r) : "w"(x));
  return r;
#else
  return sqrtf(x);
#endif
}

/* --- reading the ground map ----------------------------------------------------------------------- */

static bool read_i32(uintptr_t addr, int32_t *out)
{
  return sys_read32(addr, (uint32_t *)out);
}

static uintptr_t map_manager(void)
{
  if (!z64hd_game || !z64hd_game->voxel_mgr_ptr || !z64hd_game->voxel_formats)
    return 0;
  uintptr_t owner = sys_ptr(sys_ptr(MAIN(z64hd_game->voxel_mgr_ptr)));
  uintptr_t inner = owner ? sys_ptr(owner + MGR_OWNER) : 0;
  return inner ? sys_ptr(inner + MGR_IN_OWNER) : 0;
}

/* one field of a level's records, as the chunk has it */
typedef struct {
  bool present;
  uintptr_t data;
  uint32_t bits; /* a record */
  int32_t first; /* the field's first bit in one */
} field_t;

typedef struct {
  uintptr_t nodes[LEVELS];
  field_t intersect, intersect_water;
  field_t deadly, depth;
  int deadly_level, depth_level; /* the deepest level whose records have the field */
  int origin[3];
} chunk_t;

static field_t level_field(uintptr_t chunk, int level, int field, uint32_t width)
{
  field_t f = { 0 };
  uintptr_t records = chunk + CHUNK_RECORDS + (uintptr_t)level * RECORDS_SIZE;
  uint32_t format;
  if (!sys_read32(records + RECORDS_BITS, &f.bits) || !sys_read32(records + RECORDS_FORMAT, &format))
    return f;
  uintptr_t layout = MAIN(z64hd_game->voxel_formats) + (format < FORMAT_COUNT ? format : 0) * FORMAT_SIZE;
  if (!sys_mapped(layout, FORMAT_SIZE, SYS_PERM_R) || ((const uint8_t *)layout)[FORMAT_PRESENT + field] != 1)
    return f;
  f.first = ((const int32_t *)(layout + FORMAT_BIT))[field];
  f.data = sys_ptr(records + RECORDS_DATA);
  f.present = f.data && f.first >= 0 && (uint32_t)f.first + width <= f.bits;
  return f;
}

static bool open_chunk(uintptr_t addr, chunk_t *chunk)
{
  if (!sys_mapped(addr, CHUNK_NODES + LEVELS * sizeof(uintptr_t), SYS_PERM_R))
    return false;
  for (int i = 0; i < 3; i++)
    chunk->origin[i] = ((const int32_t *)addr)[i];
  for (int level = 0; level < LEVELS; level++)
    chunk->nodes[level] = ((const uintptr_t *)(addr + CHUNK_NODES))[level];
  /* Probe A and Probe C read Intersect (0) and IntersectWater (19) at leaf level 7 (main+0x3209720). */
  chunk->intersect = level_field(addr, LEVELS - 1, FIELD_INTERSECT, FIELD_INTERSECT_BITS);
  chunk->intersect_water = level_field(addr, LEVELS - 1, FIELD_INTERSECT_WATER, FIELD_INTERSECT_BITS);
  chunk->deadly_level = chunk->depth_level = -1;
  for (int level = LEVELS - 1; level >= 0; level--) {
    if (chunk->deadly_level < 0 &&
        (chunk->deadly = level_field(addr, level, FIELD_DEADLY, FIELD_DEADLY_BITS)).present)
      chunk->deadly_level = level;
    if (chunk->depth_level < 0 && (chunk->depth = level_field(addr, level, FIELD_DEPTH, FIELD_DEPTH_BITS)).present)
      chunk->depth_level = level;
  }
  return true;
}

/* `width` bits of record `index`, lowest first (a field is short: it's in two bytes) */
static uint32_t field_bits(const field_t *f, uint32_t index, uint32_t width)
{
  uint64_t bit = (uint64_t)index * f->bits + (uint32_t)f->first;
  uintptr_t at = f->data + (uintptr_t)(bit >> 3);
  if (!sys_mapped(at, 2, SYS_PERM_R))
    return 0;
  const uint8_t *bytes = (const uint8_t *)at;
  return ((uint32_t)bytes[0] | (uint32_t)bytes[1] << 8) >> (bit & 7) & ((1u << width) - 1);
}

static bool cell_has(int x, int y, int z, uint8_t flag)
{
  if (x < 0 || y < 0 || z < 0 || x >= BOX_X || y >= BOX_Y || z >= BOX_Z)
    return false;
  return (cells[(y * BOX_Z + z) * BOX_X + x] & flag) != 0;
}

/* whether the cube of `size` cells at corner[] (cells of the chunk) has any of the box (or deep column below it) */
static bool reaches_box(const chunk_t *chunk, const int corner[3], int size)
{
  for (int axis = 0; axis < 3; axis++) {
    int low = chunk->origin[axis] + corner[axis] - box_min[axis];
    if (axis == 1) {
      if (low >= box_size[1] || low + size <= -5000)
        return false;
    } else if (low + size <= 0 || low >= box_size[axis]) {
      return false;
    }
  }
  return true;
}

/* a cell of the chunk at corner[], whose records are path[]'s: into the box, if it's one the chunk answers for */
static void keep_cell(const chunk_t *chunk, const int corner[3], const uint32_t path[LEVELS])
{
  for (int axis = 0; axis < 3; axis++)
    if (corner[axis] < CHUNK_MARGIN || corner[axis] > CHUNK_CELLS - CHUNK_MARGIN)
      return;
  int x = chunk->origin[0] + corner[0] - box_min[0], y = chunk->origin[1] + corner[1] - box_min[1];
  int z = chunk->origin[2] + corner[2] - box_min[2];
  if (x < 0 || x >= BOX_X || z < 0 || z >= BOX_Z || y >= BOX_Y)
    return;
  uint8_t mask = CELL_MAPPED;
  if (chunk->intersect.present)
    mask |= (uint8_t)field_bits(&chunk->intersect, path[LEVELS - 1], FIELD_INTERSECT_BITS);
  if (chunk->intersect_water.present)
    mask |= (uint8_t)field_bits(&chunk->intersect_water, path[LEVELS - 1], FIELD_INTERSECT_BITS);
  if (chunk->deadly_level >= 0 && chunk->depth_level >= 0 &&
      field_bits(&chunk->deadly, path[chunk->deadly_level], FIELD_DEADLY_BITS) &&
      field_bits(&chunk->depth, path[chunk->depth_level], FIELD_DEPTH_BITS) > DEADLY_OVER) {
    mask |= CELL_DEADLY;
    if (y >= 0)
      layer_deadly[y] = true;
  }
  if (y < 0) {
    int wy = chunk->origin[1] + corner[1];
    if (wy > -32768 && wy < 32767) {
      if (wy > below_y[z][x]) {
        below_y[z][x] = (int16_t)wy;
        below_flags[z][x] = mask;
      } else if (wy == below_y[z][x]) {
        below_flags[z][x] |= mask;
      }
    }
    return;
  }
  cells[(y * BOX_Z + z) * BOX_X + x] |= mask;
  layer_mapped[y] = true;
}

/* The node `index` of `level`, whose corner is at at[] (cells of the chunk): its children that reach into the
 * box, down to the cells. path[]: the record index at each level above. */
static void walk(const chunk_t *chunk, int level, uint32_t index, const int at[3], uint32_t path[LEVELS])
{
  uintptr_t word_at = chunk->nodes[level] + (uintptr_t)index * sizeof(uint32_t);
  if (!chunk->nodes[level] || !sys_mapped(word_at, sizeof(uint32_t), SYS_PERM_R))
    return;
  uint32_t word = *(const uint32_t *)word_at;
  uint32_t mask = word & NODE_CHILD_MASK, first = word >> NODE_FIRST_SHIFT;
  int size = CHUNK_CELLS >> (level + 1); /* a child's, in cells */
  for (int child = 0; child < NODE_CHILDREN; child++) {
    if (!(mask >> child & 1))
      continue;
    int corner[3] = { at[0] + (child & 1) * size, at[1] + (child >> 1 & 1) * size, at[2] + (child >> 2 & 1) * size };
    if (!reaches_box(chunk, corner, size))
      continue;
    /* the children there are come one after another: this one's place is the count of those before it */
    path[level] = first + (uint32_t)__builtin_popcount(mask & ((1u << child) - 1));
    if (level < LEVELS - 1)
      walk(chunk, level + 1, path[level], corner, path);
    else
      keep_cell(chunk, corner, path);
  }
}

static void read_chunk(uintptr_t addr)
{
  chunk_t chunk;
  const int root[3] = { 0, 0, 0 };
  if (!open_chunk(addr, &chunk) || !reaches_box(&chunk, root, CHUNK_CELLS))
    return;
  uint32_t path[LEVELS] = { 0 };
  walk(&chunk, 0, 0, root, path);
}

/* The cells of the box round `middle`, out of every loaded chunk that reaches it. A map can have well over a
 * hundred chunks loaded, most of them far off. */
static void read_cells(const int middle[3])
{
  for (int axis = 0; axis < 3; axis++)
    box_min[axis] = middle[axis] - box_middle[axis];
  memset(cells, 0, sizeof(cells));
  memset(layer_mapped, 0, sizeof(layer_mapped));
  memset(layer_deadly, 0, sizeof(layer_deadly));
  for (int z = 0; z < BOX_Z; z++)
    for (int x = 0; x < BOX_X; x++) {
      below_y[z][x] = -32768;
      below_flags[z][x] = 0;
    }
  box_valid = false;
  uintptr_t mgr = map_manager();
  int32_t node_offset;
  if (!mgr || !read_i32(mgr + MGR_NODE_OFFSET, &node_offset))
    return;
  int seen = 0;
  uintptr_t node = sys_ptr(mgr + MGR_LIST_FIRST);
  for (int area = 0; node && node != mgr + MGR_LIST_HEAD && area < MAX_AREAS; area++) {
    uintptr_t holder = sys_ptr(node - node_offset + AREA_HOLDER);
    uintptr_t list = holder ? sys_ptr(holder + HOLDER_CHUNKS) : 0;
    int32_t count;
    if (list && read_i32(holder + HOLDER_COUNT, &count))
      for (int k = 0; k < count && seen < MAX_CHUNKS; k++, seen++) {
        uintptr_t chunk = sys_ptr(list + (uintptr_t)k * sizeof(uintptr_t));
        if (chunk) {
          read_chunk(chunk);
          box_valid = true;
        }
      }
    node = sys_ptr(node + NODE_NEXT);
  }
  /* Project any deep deadly cell (> BOX_BELOW below Link, unoccluded by safe mapped ground) into layer 1
   * (without setting CELL_MAPPED) so its red void column still rises up to Link's height. */
  if (box_valid)
    for (int z = 1; z < BOX_Z - 1; z++)
      for (int x = 1; x < BOX_X - 1; x++)
        if ((below_flags[z][x] & CELL_DEADLY) && !cell_has(x, 0, z, CELL_MAPPED) &&
            !cell_has(x, 1, z, CELL_MAPPED)) {
          cells[(1 * BOX_Z + z) * BOX_X + x] |= CELL_DEADLY;
          layer_deadly[1] = true;
        }
}

/* --- the outlines ------------------------------------------------------------------------------- */

typedef bool (*kind_t)(int x, int y, int z);

/* what outline() draws round */
typedef struct {
  kind_t kind;
  const bool *layers; /* whether each layer has any cell of the kind */
  uint8_t colour;
  bool columns;       /* each cell stands for the space over it, up to column_top: walls, not only a line */
} outline_t;

/* one side of one row of cells: the row runs along x (along 0, at z = row) or along z (along 1, at x = row), in
 * layer y; the side toward less (0) or more (1) */
typedef struct { int y, along, row, side; } row_side_t;

/* mapped ground: a mapped cell with none under it */
static bool is_ground(int x, int y, int z)
{
  return cell_has(x, y, z, CELL_MAPPED) && !cell_has(x, y - 1, z, CELL_MAPPED);
}

static bool is_deadly(int x, int y, int z)
{
  return cell_has(x, y, z, CELL_DEADLY);
}

/* the same kind of cell next door, level or a step up or down: the ground goes on there */
static bool goes_on(kind_t kind, int x, int y, int z)
{
  return kind(x, y - 1, z) || kind(x, y, z) || kind(x, y + 1, z);
}

/* True if Probe A's centre downward cast at (x, z) from below `top` finds no safe mapped ground
 * (meaning Probe A falls through to its 8 outer +/-0.6 rays, expanding an adjacent deadly cell by 0.6). */
static bool col_unmapped(int x, int z, int top)
{
  if (x < 0 || x >= BOX_X || z < 0 || z >= BOX_Z)
    return false;
  if (top > BOX_Y)
    top = BOX_Y;
  for (int iy = top - 1; iy >= 0; iy--) {
    uint8_t c = cells[(iy * BOX_Z + z) * BOX_X + x];
    if (c & CELL_MAPPED)
      return (c & CELL_DEADLY) != 0;
  }
  if (below_flags[z][x] & CELL_MAPPED)
    return (below_flags[z][x] & CELL_DEADLY) != 0;
  return true;
}

/* a line between two points in box-cell coordinates; y in cells too, lifted */
static void add_edge_f(float x0, int y0, float z0, float x1, int y1, float z1, uint8_t colour)
{
  if (n_edges >= MAX_EDGES)
    return;
  edges[n_edges++] = (edge_t){
    { (float)box_min[0] + x0, (float)(box_min[1] + y0) + LIFT, (float)box_min[2] + z0 },
    { (float)box_min[0] + x1, (float)(box_min[1] + y1) + LIFT, (float)box_min[2] + z1 }, colour };
}

/* a column's side as a face: from (x0, z0)-(x1, z1) at y0 up to y1, in box-cell coordinates */
static void add_wall_f(float x0, float z0, float x1, float z1, int y0, int y1, uint8_t colour)
{
  if (n_walls >= MAX_WALLS)
    return;
  walls[n_walls++] = (aw_face_t){
    { (float)box_min[0] + x0, (float)(box_min[1] + y0) + LIFT, (float)box_min[2] + z0 },
    { x1 - x0, 0, z1 - z0 }, { 0, (float)(y1 - y0), 0 }, colour, true };
}

/* an upright, once: two sides of a column meet in one (quantized to 0.1 cell so +/-0.6 corners dedupe) */
static void add_post_f(float x, float z, int y0, int y1, uint8_t colour)
{
  uint32_t qx = (uint32_t)(int)(x * 10.0f + 100.5f) & 0x3FFu;
  uint32_t qz = (uint32_t)(int)(z * 10.0f + 100.5f) & 0x3FFu;
  uint32_t key = (qx << 22) | (qz << 12) | ((uint32_t)(y0 & 0x3F) << 6) | (uint32_t)(y1 & 0x3F);
  if (!key)
    key = 1;
  uint32_t slot = key * HASH_MULTIPLIER >> (32 - POST_SLOT_BITS);
  while (posts[slot] && posts[slot] != key)
    slot = (slot + 1) & (POST_SLOTS - 1);
  if (posts[slot] || n_posts >= MAX_POSTS)
    return;
  posts[slot] = key;
  n_posts++;
  add_edge_f(x, y0, z, x, y1, z, colour);
}

/* where the column over a cell ends: the next mapped cell up, or the box's top */
static int column_top(int x, int y, int z)
{
  int top = y + 1;
  while (top < BOX_Y && !cell_has(x, top, z, CELL_MAPPED))
    top++;
  return top;
}

/* the cells `from` to `to` of a row are open on its side: the line there, and for columns their wall up to top.
 * When `pad` is set (deadly column facing unmapped space), push the wall outward by DEADLY_PAD (0.6) to match
 * Probe A's 3x3 outer-ray reach, mitering outer corners into unmapped space and trimming concave inner corners. */
static void add_run(const outline_t *o, const row_side_t *r, int from, int to, int top, bool pad)
{
  float edge = (float)(r->row + r->side);
  float s0 = (float)from, s1 = (float)to;
  if (pad) {
    int beside = r->row + (r->side ? 1 : -1);
    edge += r->side ? DEADLY_PAD : -DEADLY_PAD;
    int px = r->along ? r->row : from - 1, pz = r->along ? from - 1 : r->row;
    int bpx = r->along ? beside : from - 1, bpz = r->along ? from - 1 : beside;
    if (!goes_on(o->kind, px, r->y, pz)) {
      if (col_unmapped(px, pz, top))
        s0 -= DEADLY_PAD;
    } else if (goes_on(o->kind, bpx, r->y, bpz)) {
      s0 += DEADLY_PAD;
    }
    int nx = r->along ? r->row : to, nz = r->along ? to : r->row;
    int bnx = r->along ? beside : to, bnz = r->along ? to : beside;
    if (!goes_on(o->kind, nx, r->y, nz)) {
      if (col_unmapped(nx, nz, top))
        s1 += DEADLY_PAD;
    } else if (goes_on(o->kind, bnx, r->y, bnz)) {
      s1 -= DEADLY_PAD;
    }
    if (s1 <= s0 + 0.05f)
      return;
  }
  float x0 = r->along ? edge : s0, z0 = r->along ? s0 : edge;
  float x1 = r->along ? edge : s1, z1 = r->along ? s1 : edge;
  add_edge_f(x0, r->y, z0, x1, r->y, z1, o->colour);
  if (!o->columns)
    return;
  add_edge_f(x0, top, z0, x1, top, z1, o->colour);
  add_post_f(x0, z0, r->y, top, o->colour);
  add_post_f(x1, z1, r->y, top, o->colour);
  add_wall_f(x0, z0, x1, z1, r->y, top, o->colour);
}

/* One side of a row: where a cell of the kind has none beside it there, that side, the cells in a row joined
 * into one line (for columns, cut where the top or +/-0.6 pad changes). The box's own border isn't an edge:
 * what's past it isn't known. */
static void outline_row_side(const outline_t *o, const row_side_t *r)
{
  int length = box_size[r->along ? 2 : 0], beside = r->row + (r->side ? 1 : -1);
  int run = -1, run_top = 0;
  bool run_pad = false;
  for (int i = 1; i < length; i++) {
    int x = r->along ? r->row : i, z = r->along ? i : r->row;
    int beside_x = r->along ? beside : i, beside_z = r->along ? i : beside;
    bool open = i < length - 1 && o->kind(x, r->y, z) && !goes_on(o->kind, beside_x, r->y, beside_z);
    int top = open && o->columns ? column_top(x, r->y, z) : r->y;
    bool pad = open && o->columns && col_unmapped(beside_x, beside_z, top);
    if (run >= 0 && (!open || top != run_top || pad != run_pad)) {
      add_run(o, r, run, i, run_top, run_pad);
      run = -1;
    }
    if (open && run < 0) {
      run = i;
      run_top = top;
      run_pad = pad;
    }
  }
}

static void outline_layer(const outline_t *o, int y)
{
  for (int along = 0; along < 2; along++) {
    int rows = box_size[along ? 0 : 2];
    for (int row = 1; row < rows - 1; row++)
      for (int side = 0; side < 2; side++) {
        row_side_t r = { y, along, row, side };
        outline_row_side(o, &r);
      }
  }
}

/* The outline of the kind's cells, Link's layer first, then the ones over and under it by turns: if the room
 * for edges runs out, it's the far layers' that are missing. */
static void outline(const outline_t *o)
{
  for (int step = 0; step < 2 * BOX_Y; step++) {
    int y = box_middle[1] + (step & 1 ? -(step + 1) / 2 : step / 2);
    if (y >= 1 && y < BOX_Y - 1 && o->layers[y])
      outline_layer(o, y);
  }
}

static void invalidate_shift_eval(void)
{
  for (int gz = 0; gz < SHIFT_GRID; gz++)
    for (int gx = 0; gx < SHIFT_GRID; gx++)
      shift_grid[gz][gx].eval_ok = 0;
  shift_quads_dirty = true;
}

static void rebuild(const int middle[3])
{
  read_cells(middle);
  n_edges = 0;
  n_posts = 0;
  n_walls = 0;
  memset(posts, 0, sizeof(posts));
  const outline_t columns = { is_deadly, layer_deadly, AW_VOID_DEADLY, true };
  const outline_t ground = { is_ground, layer_mapped, AW_VOID_GROUND, false };
  if (box_valid && voidmap_deadly)
    outline(&columns); /* first: if the edges run out, it's the ground's that go */
  if (box_valid && voidmap_ground)
    outline(&ground);
  bool full = n_edges >= MAX_EDGES || n_walls >= MAX_WALLS;
  if (full && !full_logged)
    log_printf("void out: more than fits round here (%d lines, %d faces); the farthest left out", n_edges, n_walls);
  full_logged = full;
  built_frame = frames;
  built_ground = voidmap_ground;
  built_deadly = voidmap_deadly;
  invalidate_shift_eval();
}

/* --- Link ---------------------------------------------------------------------------------------- */

typedef struct {
  float pos[3];
  float probe_pos[3]; /* live probe query origin (probe + 0xE0) */
  float probe_h;      /* probe + 0x1164 (0.6x for probe A cast start) */
  float ray_len;      /* max probe ray length ([cfg + 0x44], or 5000.0) */
  uint32_t flags;
  bool on_map;      /* mapped ground under Link */
  float landed[3];  /* on_map: where the cast down landed */
  bool outside;     /* Link is outside the probe's box (PROBE_OUTSIDE) */
  bool has_bounds;  /* VolumeStats AABB at probe +0xEC/+0xF8 is valid */
  float aabb_min[3], aabb_max[3];
  bool has_point;   /* probe C holds a cell */
  float point[3], level;
} link_state_t;

static bool link_state(link_state_t *s)
{
  uintptr_t actor = link_actor();
  uintptr_t parts = actor ? sys_ptr(actor + ACTOR_PARTS) : 0;
  uintptr_t player = parts ? sys_ptr(parts + PARTS_PLAYER) : 0, probe = parts ? sys_ptr(parts + PARTS_PROBE) : 0;
  if (!player || !probe || !sys_mapped(actor + ACTOR_POS, sizeof(s->pos), SYS_PERM_R) ||
      !sys_read32(player + PLAYER_GROUND_FLAGS, &s->flags) ||
      !sys_mapped(probe + PROBE_FLAGS, PROBE_HEIGHT + sizeof(float) - PROBE_FLAGS, SYS_PERM_R))
    return false;
  memcpy(s->pos, (const void *)(actor + ACTOR_POS), sizeof(s->pos));
  memcpy(s->probe_pos, (const void *)(probe + PROBE_POS), sizeof(s->probe_pos));
  memcpy(s->aabb_max, (const void *)(probe + PROBE_AABB_MAX), sizeof(s->aabb_max));
  memcpy(s->aabb_min, (const void *)(probe + PROBE_AABB_MIN), sizeof(s->aabb_min));
  s->has_bounds = s->aabb_min[0] < s->aabb_max[0] && s->aabb_min[2] < s->aabb_max[2] &&
                  s->aabb_min[0] > -1e6f && s->aabb_max[0] < 1e6f && s->aabb_min[1] > -1e6f &&
                  s->aabb_min[1] < 1e6f && s->aabb_min[2] > -1e6f && s->aabb_max[2] < 1e6f;
  memcpy(&s->probe_h, (const void *)(probe + PROBE_HEIGHT), sizeof(s->probe_h));
  s->ray_len = PROBE_DEFAULT_RAY;
  uintptr_t owner = sys_ptr(probe + PROBE_OWNER);
  uintptr_t cfg = owner ? sys_ptr(owner + PROBE_OWNER_CFG) : 0;
  if (cfg && sys_mapped(cfg + PROBE_CFG_RAY_LEN, PROBE_CFG_READY + 1 - PROBE_CFG_RAY_LEN, SYS_PERM_R) &&
      (*(const uint8_t *)(cfg + PROBE_CFG_READY) & 1)) {
    float rl;
    memcpy(&rl, (const void *)(cfg + PROBE_CFG_RAY_LEN), sizeof(rl));
    if (rl > 0.0f && rl <= PROBE_DEFAULT_RAY)
      s->ray_len = rl;
  }
  s->outside = *(const uint32_t *)(probe + PROBE_FLAGS) & PROBE_OUTSIDE;
  s->on_map = *(const uint8_t *)(probe + PROBE_A_HIT) & 1;
  memcpy(s->landed, (const void *)(probe + PROBE_A_POINT), sizeof(s->landed));
  s->has_point = *(const uint8_t *)(probe + PROBE_C_HIT) & 1;
  memcpy(s->point, (const void *)(probe + PROBE_C_POINT), sizeof(s->point));
  memcpy(&s->level, (const void *)(probe + PROBE_C_LEVEL), sizeof(s->level));
  return true;
}

static int to_cell(float v)
{
  int i = (int)v;
  return v < (float)i ? i - 1 : i;
}

/* --- Probe C 64-ray simulation & ground shift assessment ----------------------------------------- */

/* Build the 64 Fibonacci-sphere unit directions used by main+0x38B668..0x38B720. */
static void init_probe_rays(void)
{
  if (probe_rays_ready)
    return;
  for (int i = 0; i < PROBE_RAYS; i++) {
    float y = 1.0f - (float)(2 * i + 1) / (float)PROBE_RAYS;
    float r2 = 1.0f - y * y;
    float r = r2 > 0.0f ? fsqrtf(r2) : 0.0f;
    float theta = (float)i * GOLDEN_ANGLE;
    float dx = r * cosf(theta), dy = y, dz = r * sinf(theta);
    float len = fsqrtf(dx * dx + dy * dy + dz * dz);
    if (len > 1e-6f) {
      probe_rays[i][0] = dx / len;
      probe_rays[i][1] = dy / len;
      probe_rays[i][2] = dz / len;
    }
  }
  probe_rays_ready = true;
}

/* Exact 3D DDA through our local VolumeStats cell box, matching main+0x543240 + filter main+0x3209720:
 * a ray hits when it exits a mapped leaf cell across a face whose (Intersect | IntersectWater) bit is set,
 * at distance <= max_dist. Face bits: 0=-X, 1=+X, 2=-Y, 3=+Y, 4=-Z, 5=+Z. */
static bool voxel_raycast(const float origin[3], const float dir[3], float max_dist, float *hit_t, int hit_cell[3])
{
  if (!box_valid || max_dist <= 0.0f)
    return false;
  float o[3] = {
    origin[0] - (float)box_min[0] + 1e-4f,
    origin[1] - (float)box_min[1] + 1e-4f,
    origin[2] - (float)box_min[2] + 1e-4f,
  };
  float t_min = 0.0f, t_max = max_dist;
  for (int a = 0; a < 3; a++) {
    if (absf(dir[a]) < 1e-7f) {
      if (o[a] < 0.0f || o[a] >= (float)box_size[a])
        return false;
    } else {
      float inv = 1.0f / dir[a];
      float t0 = -o[a] * inv, t1 = ((float)box_size[a] - o[a]) * inv;
      if (t0 > t1) {
        float tmp = t0;
        t0 = t1;
        t1 = tmp;
      }
      if (t0 > t_min)
        t_min = t0;
      if (t1 < t_max)
        t_max = t1;
      if (t_min > t_max)
        return false;
    }
  }
  int ix = to_cell(o[0] + dir[0] * t_min);
  int iy = to_cell(o[1] + dir[1] * t_min);
  int iz = to_cell(o[2] + dir[2] * t_min);
  if (ix < 0) ix = 0; else if (ix >= BOX_X) ix = BOX_X - 1;
  if (iy < 0) iy = 0; else if (iy >= BOX_Y) iy = BOX_Y - 1;
  if (iz < 0) iz = 0; else if (iz >= BOX_Z) iz = BOX_Z - 1;

  int step_x = dir[0] > 0.0f ? 1 : -1, face_x = dir[0] > 0.0f ? 1 : 0;
  int step_y = dir[1] > 0.0f ? 1 : -1, face_y = dir[1] > 0.0f ? 3 : 2;
  int step_z = dir[2] > 0.0f ? 1 : -1, face_z = dir[2] > 0.0f ? 5 : 4;
  float tx, ty, tz, dtx, dty, dtz;
  if (absf(dir[0]) < 1e-7f) {
    tx = dtx = 1e30f;
  } else {
    dtx = absf(1.0f / dir[0]);
    tx = ((dir[0] > 0.0f ? (float)(ix + 1) : (float)ix) - o[0]) / dir[0];
    if (tx < t_min) tx = t_min;
  }
  if (absf(dir[1]) < 1e-7f) {
    ty = dty = 1e30f;
  } else {
    dty = absf(1.0f / dir[1]);
    ty = ((dir[1] > 0.0f ? (float)(iy + 1) : (float)iy) - o[1]) / dir[1];
    if (ty < t_min) ty = t_min;
  }
  if (absf(dir[2]) < 1e-7f) {
    tz = dtz = 1e30f;
  } else {
    dtz = absf(1.0f / dir[2]);
    tz = ((dir[2] > 0.0f ? (float)(iz + 1) : (float)iz) - o[2]) / dir[2];
    if (tz < t_min) tz = t_min;
  }

  for (int step = 0; step < BOX_X + BOX_Y + BOX_Z; step++) {
    float t_next;
    int face, axis;
    if (tx <= ty && tx <= tz) {
      t_next = tx; face = face_x; axis = 0;
    } else if (ty <= tz) {
      t_next = ty; face = face_y; axis = 1;
    } else {
      t_next = tz; face = face_z; axis = 2;
    }
    if (t_next > max_dist)
      return false;
    uint8_t c = cells[(iy * BOX_Z + iz) * BOX_X + ix];
    if ((c & CELL_MAPPED) && (c & (1u << face))) {
      if (hit_t)
        *hit_t = t_next;
      if (hit_cell) {
        hit_cell[0] = box_min[0] + ix;
        hit_cell[1] = box_min[1] + iy;
        hit_cell[2] = box_min[2] + iz;
      }
      return true;
    }
    if (axis == 0) {
      ix += step_x;
      if (ix < 0 || ix >= BOX_X) return false;
      tx += dtx;
    } else if (axis == 1) {
      iy += step_y;
      if (iy < 0 || iy >= BOX_Y) return false;
      ty += dty;
    } else {
      iz += step_z;
      if (iz < 0 || iz >= BOX_Z) return false;
      tz += dtz;
    }
  }
  return false;
}

/* Probe A's downward cast at one column (filter main+0x32099A0): hits on -Y (bit 2), stops on +Y (bit 3)
 * after the starting cell. */
static bool probe_a_column(float wx, float wy_start, float wz, int out_cell[3])
{
  int ix = to_cell(wx + 1e-4f) - box_min[0];
  int iy = to_cell(wy_start + 1e-4f) - box_min[1];
  int iz = to_cell(wz + 1e-4f) - box_min[2];
  if (ix < 0 || ix >= BOX_X || iz < 0 || iz >= BOX_Z)
    return false;
  if (iy >= BOX_Y)
    iy = BOX_Y - 1;
  bool first = true;
  for (; iy >= 0; iy--) {
    uint8_t c = cells[(iy * BOX_Z + iz) * BOX_X + ix];
    if (c & CELL_MAPPED) {
      if (c & (1u << 2)) { /* -Y floor face */
        out_cell[0] = box_min[0] + ix;
        out_cell[1] = box_min[1] + iy;
        out_cell[2] = box_min[2] + iz;
        return true;
      }
      if (!first && (c & (1u << 3))) /* +Y ceiling blocks the cast */
        return false;
    }
    first = false;
  }
  return false;
}

/* Probe A's 9-point downward check (centre, then 8 offsets at +/-0.6 in X/Z, main+0x38AF50..0x38AFFC). */
static bool probe_a_sim(const float pq[3], float probe_h, int out_cell[3])
{
  static const float off[9][2] = {
    { 0.0f, 0.0f },   { -0.6f, 0.0f },  { 0.6f, 0.0f },
    { 0.0f, -0.6f },  { 0.0f, 0.6f },   { -0.6f, -0.6f },
    { -0.6f, 0.6f },  { 0.6f, -0.6f },  { 0.6f, 0.6f },
  };
  float y_start = pq[1] + 0.6f * probe_h;
  for (int k = 0; k < 9; k++)
    if (probe_a_column(pq[0] + off[k][0], y_start, pq[2] + off[k][1], out_cell))
      return true;
  return false;
}

/* True if standing at ground position (wx, wy, wz) satisfies Probe C's update condition:
 * at least one of the 64 Fibonacci-sphere rays hits an (Intersect | IntersectWater) face strictly closer
 * in 3D than Probe C's current remembered point `s->point` (and within `s->ray_len`). */
static bool will_ring_shift(const link_state_t *s, float dy_probe, float wx, float wy, float wz)
{
  float q[3] = { wx, wy + dy_probe, wz };
  /* Pre-pass (main+0x38B374..0x38B554): when Probe A hits, cast from A's cell centre toward Q; if that
   * encounters an intervening voxel face, Q snaps to that hit cell's centre. */
  int acell[3];
  bool on_map = probe_a_sim(q, s->probe_h, acell);
  if (on_map) {
    float ac[3] = { (float)acell[0] + 0.5f, (float)acell[1] + 0.5f, (float)acell[2] + 0.5f };
    float vx = q[0] - ac[0], vy = q[1] - ac[1], vz = q[2] - ac[2];
    float len = fsqrtf(vx * vx + vy * vy + vz * vz);
    if (len > 1e-4f) {
      float dir[3] = { vx / len, vy / len, vz / len };
      float ht;
      int hcell[3];
      if (voxel_raycast(ac, dir, len, &ht, hcell) && ht <= len) {
        q[0] = (float)hcell[0] + 0.5f;
        q[1] = (float)hcell[1] + 0.5f;
        q[2] = (float)hcell[2] + 0.5f;
      }
    }
  }
  float limit = s->ray_len;
  if (s->has_point) {
    float dx = q[0] - s->point[0], dy = q[1] - s->point[1], dz = q[2] - s->point[2];
    float cur = fsqrtf(dx * dx + dy * dy + dz * dz) - 1e-3f; /* strictly < current 3D distance */
    if (cur <= 0.0f)
      return false;
    if (cur < limit)
      limit = cur;
  }
  /* Test from ray 63 downward first: rays 56..63 point steepest down and hit floor directly underfoot on the
   * very first iteration when on mapped ground, avoiding the remaining rays. When off the map, find the closest
   * 3D hit and require its horizontal distance to stay < 8.0 (a closer 3D wall >= 8m sideways still voids). */
  bool found = false;
  float best_d2_sq = 0.0f;
  for (int i = PROBE_RAYS - 1; i >= 0; i--) {
    float ht;
    if (voxel_raycast(q, probe_rays[i], limit, &ht, 0)) {
      if (on_map)
        return true;
      found = true;
      limit = ht;
      float rx = probe_rays[i][0], rz = probe_rays[i][2];
      best_d2_sq = ht * ht * (rx * rx + rz * rz);
    }
  }
  return found && best_d2_sq < VOID_DISTANCE * VOID_DISTANCE;
}

/* Single downward Phive collision raycast (main+0x5FF750 / 1.0.0 main+0x582D00) on player ground layer 0x1A. */
typedef bool (*phive_raycast_fn)(const float from[3], const float to[3], void *hit, const void *ignore, int layer,
                                 int unk);

static bool phive_raycast_ready(void)
{
  if (!z64hd_game || !z64hd_game->phive_raycast || !z64hd_game->vt_raycast_hit || !z64hd_game->vt_raycast_filter ||
      !z64hd_game->phive_mgr_ptr)
    return false;
  uintptr_t mgr = sys_ptr(sys_ptr(MAIN(z64hd_game->phive_mgr_ptr)));
  uintptr_t sub = mgr ? sys_ptr(mgr + 0x18) : 0;
  return sub && sys_ptr(sub + 0x48);
}

static bool phive_ground_cast(const float from[3], const float to[3], float out_pos[3], float out_norm[3])
{
  uint64_t hit_buf[0x180 / 8];
  uint8_t *hit = (uint8_t *)hit_buf;
  memset(hit, 0, 0x180);
  *(uintptr_t *)(hit + 0x000) = MAIN(z64hd_game->vt_raycast_hit);
  *(float *)(hit + 0x03C) = -1.0f;
  *(uintptr_t *)(hit + 0x060) = (uintptr_t)(hit + 0x128);
  *(uint32_t *)(hit + 0x090) = 1u;
  *(uint32_t *)(hit + 0x114) = 1u;
  *(uintptr_t *)(hit + 0x128) = MAIN(z64hd_game->vt_raycast_filter);
  *(uint32_t *)(hit + 0x130) = 3u;
  *(uint64_t *)(hit + 0x138) = 0x003FFFFFull;
  *(uint64_t *)(hit + 0x140) = 0x07FFFFFFEEFFFFFFull;
  *(uint64_t *)(hit + 0x148) = 0x003FFFFFull;
  *(uint64_t *)(hit + 0x150) = 0x07FFFFFFEEFFFFFFull;
  *(uint64_t *)(hit + 0x158) = 1ull;
  phive_raycast_fn fn = (phive_raycast_fn)MAIN(z64hd_game->phive_raycast);
  if (!fn(from, to, hit, 0, PHIVE_LAYER_GROUND, 0))
    return false;
  memcpy(out_pos, hit + 0x24, sizeof(float) * 3);
  memcpy(out_norm, hit + 0x30, sizeof(float) * 3);
  return true;
}

/* Sample walkable collision ground at (wx, wz) near Link's height ref_y. Two-stage cast: first from just above
 * Link's head (stays under dungeon ceilings/bridges), then higher up if missed (catches steep uphill slopes). */
static void sample_ground_cell(const link_state_t *s, bool use_phive, float ref_y, float wx, float wz,
                               shift_cell_t *c)
{
  c->sampled = 1;
  c->has_ground = 0;
  c->state = SHIFT_NONE;
  c->eval_ok = 0;
  if (use_phive) {
    float pos[3], norm[3];
    float from1[3] = { wx, ref_y + 2.8f, wz }, to1[3] = { wx, ref_y - 12.0f, wz };
    bool ok = phive_ground_cast(from1, to1, pos, norm) && norm[1] > SHIFT_WALK_NY && pos[1] < ref_y + 2.75f;
    /* Avoid landing on Link's own collision capsule right at his feet. */
    if (ok && absf(wx - s->pos[0]) < 0.65f && absf(wz - s->pos[2]) < 0.65f && pos[1] > s->pos[1] + 0.25f) {
      float from_low[3] = { wx, s->pos[1] + 0.15f, wz };
      ok = phive_ground_cast(from_low, to1, pos, norm) && norm[1] > SHIFT_WALK_NY;
      if (!ok) {
        pos[1] = s->pos[1];
        norm[0] = 0.0f; norm[1] = 1.0f; norm[2] = 0.0f;
        ok = true;
      }
    }
    if (!ok) {
      float from2[3] = { wx, ref_y + 9.0f, wz }, to2[3] = { wx, ref_y + 2.2f, wz };
      ok = phive_ground_cast(from2, to2, pos, norm) && norm[1] > SHIFT_WALK_NY && pos[1] < ref_y + 8.95f;
    }
    if (ok) {
      c->y = pos[1];
      c->nx = norm[0];
      c->ny = norm[1];
      c->nz = norm[2];
      c->has_ground = 1;
    }
    return;
  }
  /* Fallback if Phive raycast isn't available on this build: highest mapped ground cell near Link. */
  int ix = to_cell(wx) - box_min[0], iz = to_cell(wz) - box_min[2];
  if (ix < 0 || ix >= BOX_X || iz < 0 || iz >= BOX_Z)
    return;
  int best = -1, best_d = BOX_Y;
  int mid_y = to_cell(ref_y) - box_min[1];
  for (int iy = 1; iy < BOX_Y; iy++) {
    if (is_ground(ix, iy, iz)) {
      int d = iy > mid_y ? iy - mid_y : mid_y - iy;
      if (d < best_d) {
        best_d = d;
        best = iy;
      }
    }
  }
  if (best >= 0) {
    c->y = (float)(box_min[1] + best);
    c->nx = 0.0f; c->ny = 1.0f; c->nz = 0.0f;
    c->has_ground = 1;
  }
}

/* Rebuild the merged ground quads from shift_grid[][], ordered by distance ring from Link (nearest first). */
static void build_shift_quads(void)
{
  n_shift_quads = 0;
  const float ring_w = (float)(SHIFT_RADIUS + 1) / (float)SHIFT_RINGS;
  for (int ring_no = 0; ring_no < SHIFT_RINGS; ring_no++) {
    for (int gz = 0; gz < SHIFT_GRID; gz++) {
      int dz = gz - SHIFT_RADIUS;
      float wz0 = (float)(shift_oz + dz);
      for (int gx = 0; gx < SHIFT_GRID; ) {
        shift_cell_t *c = &shift_grid[gz][gx];
        int dx = gx - SHIFT_RADIUS;
        if (c->state == SHIFT_NONE || (float)(dx * dx + dz * dz) > SHIFT_RADIUS_SQ) {
          gx++;
          continue;
        }
        /* Flat tiles of the same colour and height along +X merge into one quad (saves GPU quads). */
        if (c->ny > SHIFT_FLAT_NY) {
          int run_end = gx + 1;
          float sum_y = c->y;
          while (run_end < SHIFT_GRID) {
            shift_cell_t *n = &shift_grid[gz][run_end];
            int ndx = run_end - SHIFT_RADIUS;
            if (n->state != c->state || n->ny <= SHIFT_FLAT_NY ||
                (float)(ndx * ndx + dz * dz) > SHIFT_RADIUS_SQ || absf(n->y - c->y) > SHIFT_MERGE_DY)
              break;
            sum_y += n->y;
            run_end++;
          }
          int len = run_end - gx;
          float mid_dx = (float)dx + (float)len * 0.5f - 0.5f;
          int r = (int)(fsqrtf(mid_dx * mid_dx + (float)(dz * dz)) / ring_w);
          if (r >= SHIFT_RINGS) r = SHIFT_RINGS - 1;
          if (r == ring_no && n_shift_quads < SHIFT_GRID * SHIFT_GRID) {
            float y = sum_y / (float)len + SHIFT_GROUND_LIFT;
            uint8_t col = c->state == SHIFT_OK ? AW_VOID_SHIFT_OK : AW_VOID_SHIFT_FAIL;
            shift_quads[n_shift_quads++] = (aw_face_t){
              { (float)(shift_ox + dx), y, wz0 },
              { (float)len, 0.0f, 0.0f },
              { 0.0f, 0.0f, 1.0f },
              col, true
            };
          }
          gx = run_end;
        } else {
          int r = (int)(fsqrtf((float)(dx * dx + dz * dz)) / ring_w);
          if (r >= SHIFT_RINGS) r = SHIFT_RINGS - 1;
          if (r == ring_no && n_shift_quads < SHIFT_GRID * SHIFT_GRID) {
            float sx = -c->nx / c->ny, sz = -c->nz / c->ny;
            if (sx < -SHIFT_MAX_SLOPE) sx = -SHIFT_MAX_SLOPE;
            else if (sx > SHIFT_MAX_SLOPE) sx = SHIFT_MAX_SLOPE;
            if (sz < -SHIFT_MAX_SLOPE) sz = -SHIFT_MAX_SLOPE;
            else if (sz > SHIFT_MAX_SLOPE) sz = SHIFT_MAX_SLOPE;
            float base_y = c->y - 0.5f * (sx + sz) + SHIFT_GROUND_LIFT;
            uint8_t col = c->state == SHIFT_OK ? AW_VOID_SHIFT_OK : AW_VOID_SHIFT_FAIL;
            shift_quads[n_shift_quads++] = (aw_face_t){
              { (float)(shift_ox + dx), base_y, wz0 },
              { 1.0f, sx, 0.0f },
              { 0.0f, sz, 1.0f },
              col, true
            };
          }
          gx++;
        }
      }
    }
  }
  shift_quads_dirty = false;
}

static void update_shift_grid(const link_state_t *s, const int here[3])
{
  init_probe_rays();
  if (!box_valid) {
    shift_grid_valid = false;
    n_shift_quads = 0;
    return;
  }
  float dy_probe = s->probe_pos[1] - s->pos[1];
  if (!(dy_probe >= -0.5f && dy_probe <= 3.0f))
    dy_probe = PROBE_DEFAULT_DY;

  bool need_full = !shift_grid_valid || (frames - shift_height_frame >= REBUILD_FRAMES) ||
                   absf(s->pos[1] - shift_ref_y) > 2.5f;
  int dgx = here[0] - shift_ox, dgz = here[2] - shift_oz;
  if (!need_full && (dgx != 0 || dgz != 0)) {
    if (dgx < -4 || dgx > 4 || dgz < -4 || dgz > 4) {
      need_full = true;
    } else {
      /* Shift cached ground samples and only raycast the newly exposed border cells. */
      memset(shift_tmp, 0, sizeof(shift_tmp));
      for (int gz = 0; gz < SHIFT_GRID; gz++) {
        int oz = gz + dgz;
        if (oz < 0 || oz >= SHIFT_GRID) continue;
        for (int gx = 0; gx < SHIFT_GRID; gx++) {
          int ox = gx + dgx;
          if (ox < 0 || ox >= SHIFT_GRID) continue;
          shift_tmp[gz][gx] = shift_grid[oz][ox];
        }
      }
      memcpy(shift_grid, shift_tmp, sizeof(shift_grid));
      shift_ox = here[0];
      shift_oz = here[2];
      shift_quads_dirty = true;
    }
  }
  if (need_full) {
    memset(shift_grid, 0, sizeof(shift_grid));
    shift_ox = here[0];
    shift_oz = here[2];
    shift_ref_y = s->pos[1];
    shift_height_frame = frames;
    shift_grid_valid = true;
    shift_quads_dirty = true;
  }

  /* If Probe C's anchor moved (or appeared/disappeared) or dy changed noticeably, re-evaluate all cells. */
  bool anchor_changed = s->has_point != shift_anchor_had || absf(dy_probe - shift_eval_dy) > 0.15f;
  if (!anchor_changed && s->has_point) {
    float adx = s->point[0] - shift_anchor[0], ady = s->point[1] - shift_anchor[1],
          adz = s->point[2] - shift_anchor[2];
    if (adx * adx + ady * ady + adz * adz > 0.05f * 0.05f)
      anchor_changed = true;
  }
  if (anchor_changed) {
    invalidate_shift_eval();
    shift_anchor_had = s->has_point;
    memcpy(shift_anchor, s->point, sizeof(shift_anchor));
    shift_eval_dy = dy_probe;
  }

  bool use_phive = phive_raycast_ready();
  for (int gz = 0; gz < SHIFT_GRID; gz++) {
    int dz = gz - SHIFT_RADIUS;
    float wz = (float)(shift_oz + dz) + 0.5f;
    for (int gx = 0; gx < SHIFT_GRID; gx++) {
      int dx = gx - SHIFT_RADIUS;
      if ((float)(dx * dx + dz * dz) > SHIFT_RADIUS_SQ)
        continue;
      shift_cell_t *c = &shift_grid[gz][gx];
      float wx = (float)(shift_ox + dx) + 0.5f;
      if (!c->sampled) {
        sample_ground_cell(s, use_phive, shift_ref_y, wx, wz, c);
        shift_quads_dirty = true;
      }
      if (c->has_ground && !c->eval_ok) {
        uint8_t next = will_ring_shift(s, dy_probe, wx, c->y, wz) ? SHIFT_OK : SHIFT_FAIL;
        if (next != c->state)
          shift_quads_dirty = true;
        c->state = next;
        c->eval_ok = 1;
      }
    }
  }
  if (shift_quads_dirty)
    build_shift_quads();
}

/* --- lines ---------------------------------------------------------------------------------------- */

static int n_out, max_out;
static float (*ends_out)[2][3];
static uint8_t *colours_out;

static void line(const float a[3], const float b[3], uint8_t colour)
{
  if (n_out >= max_out)
    return;
  memcpy(ends_out[n_out][0], a, sizeof(float) * 3);
  memcpy(ends_out[n_out][1], b, sizeof(float) * 3);
  colours_out[n_out++] = colour;
}

/* where the fence's segment j starts, level (segment RING_SEGMENTS is segment 0 again) */
static void fence_corner(int j, float *x, float *z)
{
  float angle = TWO_PI * j / RING_SEGMENTS;
  *x = fence.point[0] + VOID_DISTANCE * sinf(angle);
  *z = fence.point[2] + VOID_DISTANCE * cosf(angle);
}

/* The fence Link has to stay in while nothing is under Link: rings at the height of Link's feet and RING_REACH
 * over and under it, joined by uprights, and the way back to the point. */
static void ring(const link_state_t *s)
{
  fence.on = true;
  fence.colour = s->flags & FLAG_FAR_FROM_GROUND ? AW_VOID_DEADLY : AW_VOID_RING;
  fence.y = s->pos[1] + LIFT;
  memcpy(fence.point, s->point, sizeof(fence.point));
  for (int j = 0; j < RING_SEGMENTS; j++) {
    float x0, z0, x1, z1;
    fence_corner(j, &x0, &z0);
    fence_corner(j + 1, &x1, &z1);
    for (int level = -1; level <= 1; level++) {
      float u[3] = { x0, fence.y + level * RING_REACH, z0 }, v[3] = { x1, fence.y + level * RING_REACH, z1 };
      line(u, v, fence.colour);
    }
    if (j % RING_POST_EVERY == 0) {
      float low[3] = { x0, fence.y - RING_REACH, z0 }, high[3] = { x0, fence.y + RING_REACH, z0 };
      line(low, high, fence.colour);
    }
  }
  if (!s->on_map) {
    float feet[3] = { s->pos[0], fence.y, s->pos[2] };
    line(feet, s->point, fence.colour);
  }
}

/* The edges nearest Link, as many as there's room for: a first pass counts them by distance, the second takes
 * the ones within the distance that fits. */
static void nearest_edges(const link_state_t *s, int room)
{
  const float reach = BOX_X; /* farther than any edge is */
  int counts[EDGE_STEPS] = { 0 };
  for (int i = 0; i < n_edges; i++) {
    float dx = (edges[i].a[0] + edges[i].b[0]) / 2 - s->pos[0], dy = (edges[i].a[1] - s->pos[1]) * HEIGHT_COUNTS;
    float dz = (edges[i].a[2] + edges[i].b[2]) / 2 - s->pos[2];
    int step = (int)(sqrtf(dx * dx + dy * dy + dz * dz) / reach * EDGE_STEPS);
    edge_step[i] = (uint8_t)(step < EDGE_STEPS ? step : EDGE_STEPS - 1);
    counts[edge_step[i]]++;
  }
  int last = 0, total = 0;
  while (last < EDGE_STEPS && total + counts[last] <= room)
    total += counts[last++];
  for (int i = 0; i < n_edges; i++)
    if (edge_step[i] < last)
      line(edges[i].a, edges[i].b, edges[i].colour);
}

/* whether the box's cells are due to be read again: Link has left its middle, time has passed, or what's
 * outlined has changed */
static bool rebuild_due(const int here[3])
{
  if (!box_valid)
    return frames - built_frame >= RETRY_FRAMES;
  if (frames - built_frame >= REBUILD_FRAMES || built_ground != voidmap_ground || built_deadly != voidmap_deadly)
    return true;
  for (int axis = 0; axis < 3; axis++) {
    int off = here[axis] - (box_min[axis] + box_middle[axis]);
    if (off > REBUILD_MOVED || off < -REBUILD_MOVED)
      return true;
  }
  return false;
}

/* Draw a quad patch + wireframe border/crosshairs for one nearby plane of the VolumeStats outer AABB (+5m). */
static void add_bounds_quad(const float c[3], const float u[3], const float v[3], uint8_t col)
{
  float p1[3] = { c[0] + u[0], c[1] + u[1], c[2] + u[2] };
  float p2[3] = { p1[0] + v[0], p1[1] + v[1], p1[2] + v[2] };
  float p3[3] = { c[0] + v[0], c[1] + v[1], c[2] + v[2] };
  line(c, p1, col);
  line(p1, p2, col);
  line(p2, p3, col);
  line(p3, c, col);
  float mu0[3] = { c[0] + 0.5f * u[0], c[1] + 0.5f * u[1], c[2] + 0.5f * u[2] };
  float mu1[3] = { p3[0] + 0.5f * u[0], p3[1] + 0.5f * u[1], p3[2] + 0.5f * u[2] };
  float mv0[3] = { c[0] + 0.5f * v[0], c[1] + 0.5f * v[1], c[2] + 0.5f * v[2] };
  float mv1[3] = { p1[0] + 0.5f * v[0], p1[1] + 0.5f * v[1], p1[2] + 0.5f * v[2] };
  line(mu0, mu1, col);
  line(mv0, mv1, col);
  if (n_bounds_faces < 5)
    bounds_faces[n_bounds_faces++] = (aw_face_t){
      { c[0], c[1], c[2] }, { u[0], u[1], u[2] }, { v[0], v[1], v[2] }, col, !voidmap_xray };
}

/* Nearby planes of the map's VolumeStats outer AABB: leaving X/Z by > 5m or dropping > 5m below minY voids
 * (main+0x38CE24 sets PROBE_OUTSIDE -> bit 19). Only planes within BOUNDS_SHOW_RANGE of Link are drawn. */
static void outer_bounds(const link_state_t *s)
{
  uint8_t col = s->outside ? AW_VOID_DEADLY : AW_VOID_BOUNDS;
  float x0 = s->aabb_min[0] - PROBE_BOX_MARGIN, x1 = s->aabb_max[0] + PROBE_BOX_MARGIN;
  float y0 = s->aabb_min[1] - PROBE_BOX_MARGIN;
  float z0 = s->aabb_min[2] - PROBE_BOX_MARGIN, z1 = s->aabb_max[2] + PROBE_BOX_MARGIN;
  float y_lo = s->pos[1] - BOUNDS_WALL_HALF;
  if (y_lo < y0)
    y_lo = y0;
  float y_hi = s->pos[1] + BOUNDS_WALL_HALF;
  if (y_hi < y_lo + 1.0f)
    y_hi = y_lo + 1.0f;

  float z_lo = s->pos[2] - BOUNDS_PATCH_HALF, z_hi = s->pos[2] + BOUNDS_PATCH_HALF;
  if (z_lo < z0) z_lo = z0;
  if (z_hi > z1) z_hi = z1;
  if (z_hi > z_lo) {
    if (absf(s->pos[0] - x0) <= BOUNDS_SHOW_RANGE) {
      float c[3] = { x0, y_lo, z_lo }, u[3] = { 0, 0, z_hi - z_lo }, v[3] = { 0, y_hi - y_lo, 0 };
      add_bounds_quad(c, u, v, col);
    }
    if (absf(s->pos[0] - x1) <= BOUNDS_SHOW_RANGE) {
      float c[3] = { x1, y_lo, z_lo }, u[3] = { 0, 0, z_hi - z_lo }, v[3] = { 0, y_hi - y_lo, 0 };
      add_bounds_quad(c, u, v, col);
    }
  }

  float x_lo = s->pos[0] - BOUNDS_PATCH_HALF, x_hi = s->pos[0] + BOUNDS_PATCH_HALF;
  if (x_lo < x0) x_lo = x0;
  if (x_hi > x1) x_hi = x1;
  if (x_hi > x_lo) {
    if (absf(s->pos[2] - z0) <= BOUNDS_SHOW_RANGE) {
      float c[3] = { x_lo, y_lo, z0 }, u[3] = { x_hi - x_lo, 0, 0 }, v[3] = { 0, y_hi - y_lo, 0 };
      add_bounds_quad(c, u, v, col);
    }
    if (absf(s->pos[2] - z1) <= BOUNDS_SHOW_RANGE) {
      float c[3] = { x_lo, y_lo, z1 }, u[3] = { x_hi - x_lo, 0, 0 }, v[3] = { 0, y_hi - y_lo, 0 };
      add_bounds_quad(c, u, v, col);
    }
  }

  if (x_hi > x_lo && z_hi > z_lo && s->pos[1] - y0 <= BOUNDS_SHOW_RANGE && y0 - s->pos[1] <= BOUNDS_SHOW_RANGE) {
    float c[3] = { x_lo, y0, z_lo }, u[3] = { x_hi - x_lo, 0, 0 }, v[3] = { 0, 0, z_hi - z_lo };
    add_bounds_quad(c, u, v, col);
  }
}

int voidmap_lines(float ends[][2][3], uint8_t colours[], int max)
{
  frames++;
  fence.on = false;
  n_bounds_faces = 0;
  link_state_t s;
  if (!voidmap_shown || !map_manager() || !link_state(&s))
    return 0;
  ends_out = ends;
  colours_out = colours;
  n_out = 0;
  max_out = max;
  if (voidmap_ring && s.has_point)
    ring(&s);
  if (voidmap_bounds && s.has_bounds)
    outer_bounds(&s);
  if (voidmap_ground || voidmap_deadly || voidmap_shift) {
    int here[3] = { to_cell(s.pos[0]), to_cell(s.pos[1]), to_cell(s.pos[2]) };
    if (rebuild_due(here))
      rebuild(here);
    if (voidmap_ground || voidmap_deadly)
      nearest_edges(&s, max - n_out);
    if (voidmap_shift)
      update_shift_grid(&s, here);
  }
  return n_out;
}

/* the fence's sides, between its lowest and highest rings */
static int fence_faces(aw_face_t faces[], int max)
{
  int n = 0;
  for (int j = 0; j < RING_SEGMENTS && n < max; j++) {
    float x0, z0, x1, z1;
    fence_corner(j, &x0, &z0);
    fence_corner(j + 1, &x1, &z1);
    faces[n++] = (aw_face_t){ { x0, fence.y - RING_REACH, z0 }, { x1 - x0, 0, z1 - z0 }, { 0, 2 * RING_REACH, 0 },
                              fence.colour, !voidmap_xray };
  }
  return n;
}

/* the translucent green/red ground tiles showing where the 8-unit fence will or won't move */
static int shift_faces(aw_face_t faces[], int max)
{
  int n = n_shift_quads < max ? n_shift_quads : max;
  for (int i = 0; i < n; i++) {
    faces[i] = shift_quads[i];
    faces[i].scene = !voidmap_xray;
  }
  return n;
}

/* the columns' sides within WALL_RANGE of Link (level), the nearest ring of them first */
static int column_faces(const link_state_t *s, aw_face_t faces[], int max)
{
  const float ring_width = WALL_RANGE / WALL_RINGS;
  int n = 0;
  for (int ring_number = 0; ring_number < WALL_RINGS; ring_number++)
    for (int i = 0; i < n_walls && n < max; i++) {
      const aw_face_t *wall = &walls[i];
      float dx = wall->corner[0] + wall->u[0] / 2 - s->pos[0], dz = wall->corner[2] + wall->u[2] / 2 - s->pos[2];
      if ((int)(sqrtf(dx * dx + dz * dz) / ring_width) != ring_number)
        continue;
      faces[n] = *wall;
      faces[n++].scene = !voidmap_xray;
    }
  return n;
}

int voidmap_faces(aw_face_t faces[], int max)
{
  link_state_t s;
  if (!voidmap_shown || !map_manager() || !link_state(&s))
    return 0;
  int n = fence.on && voidmap_ring ? fence_faces(faces, max) : 0;
  if (voidmap_bounds)
    for (int i = 0; i < n_bounds_faces && n < max; i++)
      faces[n++] = bounds_faces[i];
  if (voidmap_shift && shift_grid_valid && box_valid)
    n += shift_faces(faces + n, max - n);
  if (voidmap_deadly && box_valid)
    n += column_faces(&s, faces + n, max - n);
  return n;
}

void voidmap_frame(void)
{
  link_state_t s;
  if (!voidmap_shown || !map_manager() || !link_state(&s)) {
    void_flags_were = 0;
    return;
  }
  uint32_t now = s.flags & (FLAG_DEADLY_CELL | FLAG_FAR_FROM_GROUND), set = now & ~void_flags_were;
  void_flags_were = now;
  if (set & FLAG_FAR_FROM_GROUND)
    log_printf("void out: far from ground, link %.2f %.2f %.2f, point %.2f %.2f %.2f, %.2f", (double)s.pos[0],
               (double)s.pos[1], (double)s.pos[2], (double)s.point[0], (double)s.point[1], (double)s.point[2],
               (double)s.level);
  if (set & FLAG_DEADLY_CELL && s.outside)
    log_printf("void out: outside the map's box, link %.2f %.2f %.2f", (double)s.pos[0], (double)s.pos[1],
               (double)s.pos[2]);
  else if (set & FLAG_DEADLY_CELL)
    log_printf("void out: deadly cell, link %.2f %.2f %.2f, cell at %.2f %.2f %.2f", (double)s.pos[0],
               (double)s.pos[1], (double)s.pos[2], (double)s.landed[0], (double)s.landed[1], (double)s.landed[2]);
}

void voidmap_text(char *buf, int len)
{
  link_state_t s;
  if (!z64hd_game || !z64hd_game->voxel_mgr_ptr)
    snprintf(buf, len, TF("not known for this game version"));
  else if (!map_manager() || !link_state(&s))
    snprintf(buf, len, TF("no ground map here"));
  else if (s.flags & FLAG_DEADLY_CELL)
    snprintf(buf, len, s.outside ? TF("VOID  outside the map's box") : TF("VOID  deadly cell"));
  else if (s.flags & FLAG_FAR_FROM_GROUND)
    snprintf(buf, len, TF("VOID  far from the ground"));
  else if (s.on_map)
    snprintf(buf, len, TF("on the map"));
  else if (s.has_point)
    snprintf(buf, len, TF("off the map  %.1f of %.1f"), (double)s.level, (double)VOID_DISTANCE);
  else
    snprintf(buf, len, TF("off the map  no ground found"));
}
