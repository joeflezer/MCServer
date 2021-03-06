
// VillageGen.cpp

// Implements the cVillageGen class representing the village generator

#include "Globals.h"
#include "VillageGen.h"
#include "Prefabs/AlchemistVillagePrefabs.h"
#include "Prefabs/JapaneseVillagePrefabs.h"
#include "Prefabs/PlainsVillagePrefabs.h"
#include "Prefabs/SandVillagePrefabs.h"
#include "Prefabs/SandFlatRoofVillagePrefabs.h"
#include "PieceGenerator.h"





/*
How village generating works:
By descending from a cGridStructGen, a semi-random (jitter) grid is generated. A village may be generated for each
of the grid's cells. Each cell checks the biomes in an entire chunk around it, only generating a village if all
biomes are village-friendly. If yes, the entire village structure is built for that cell. If not, the cell
is left village-less.

A village is generated using the regular BFS piece generator. The well piece is used as the starting piece,
the roads and houses are then used as the following pieces. Only the houses are read from the prefabs,
though, the roads are generated by code and their content is ignored. A special subclass of the cPiecePool
class is used, so that the roads connect to each other and to the well only in predefined manners.

The well has connectors of type "2". The houses have connectors of type "-1". The roads have connectors of
both types' opposites, type "-2" at the far ends and type "1" on the long edges. Additionally, there are
type "2" connectors along the long edges of the roads as well, so that the roads create T junctions.

When the village is about to be drawn into a chunk, it queries the heights for each piece intersecting the
chunk. The pieces are shifted so that their pivot points lie on the surface, and the roads are drawn
directly by turning the surface blocks into gravel / sandstone.

The village prefabs are stored in global piecepools (one pool per village type). In order to support
per-village density setting, the cVillage class itself implements the cPiecePool interface, relaying the
calls to the underlying cVillagePiecePool, after processing the density check.
*/

class cVillagePiecePool :
	public cPrefabPiecePool
{
	typedef cPrefabPiecePool super;
public:
	cVillagePiecePool(
		const cPrefab::sDef * a_PieceDefs,         size_t a_NumPieceDefs,
		const cPrefab::sDef * a_StartingPieceDefs, size_t a_NumStartingPieceDefs
	) :
		super(a_PieceDefs, a_NumPieceDefs, a_StartingPieceDefs, a_NumStartingPieceDefs)
	{
		// Add the road pieces:
		for (int len = 27; len < 60; len += 12)
		{
			cBlockArea BA;
			BA.Create(len, 1, 3, cBlockArea::baTypes | cBlockArea::baMetas);
			BA.Fill(cBlockArea::baTypes | cBlockArea::baMetas, E_BLOCK_GRAVEL, 0);
			cPrefab * RoadPiece = new cPrefab(BA, 1);
			RoadPiece->AddConnector(0,       0, 1, BLOCK_FACE_XM, -2);
			RoadPiece->AddConnector(len - 1, 0, 1, BLOCK_FACE_XP, -2);
			RoadPiece->SetDefaultWeight(100);
			
			// Add the road connectors:
			for (int x = 1; x < len; x += 12)
			{
				RoadPiece->AddConnector(x, 0, 0, BLOCK_FACE_ZM, 2);
				RoadPiece->AddConnector(x, 0, 2, BLOCK_FACE_ZP, 2);
			}
			
			// Add the buildings connectors:
			for (int x = 7; x < len; x += 12)
			{
				RoadPiece->AddConnector(x, 0, 0, BLOCK_FACE_ZM, 1);
				RoadPiece->AddConnector(x, 0, 2, BLOCK_FACE_ZP, 1);
			}
			m_AllPieces.push_back(RoadPiece);
			m_PiecesByConnector[-2].push_back(RoadPiece);
			m_PiecesByConnector[1].push_back(RoadPiece);
			m_PiecesByConnector[2].push_back(RoadPiece);
		}  // for len - roads of varying length
	}
	
	
	// cPrefabPiecePool overrides:
	virtual int GetPieceWeight(const cPlacedPiece & a_PlacedPiece, const cPiece::cConnector & a_ExistingConnector, const cPiece & a_NewPiece) override
	{
		// Roads cannot branch T-wise (appending -2 connector to a +2 connector on a 1-high piece):
		if ((a_ExistingConnector.m_Type == 2) && (a_PlacedPiece.GetDepth() > 0) && (a_PlacedPiece.GetPiece().GetSize().y == 1))
		{
			return 0;
		}
		
		return ((const cPrefab &)a_NewPiece).GetPieceWeight(a_PlacedPiece, a_ExistingConnector);
	}
};





class cVillageGen::cVillage :
	public cGridStructGen::cStructure,
	protected cPiecePool
{
	typedef cGridStructGen::cStructure super;
	
public:
	cVillage(
		int a_Seed,
		int a_GridX, int a_GridZ,
		int a_OriginX, int a_OriginZ,
		int a_MaxRoadDepth,
		int a_MaxSize,
		int a_Density,
		cPiecePool & a_Prefabs,
		cTerrainHeightGenPtr a_HeightGen,
		BLOCKTYPE a_RoadBlock,
		BLOCKTYPE a_WaterRoadBlock
	) :
		super(a_GridX, a_GridZ, a_OriginX, a_OriginZ),
		m_Seed(a_Seed),
		m_Noise(a_Seed),
		m_MaxSize(a_MaxSize),
		m_Density(a_Density),
		m_Borders(a_OriginX - a_MaxSize, 0, a_OriginZ - a_MaxSize, a_OriginX + a_MaxSize, cChunkDef::Height - 1, a_OriginZ + a_MaxSize),
		m_Prefabs(a_Prefabs),
		m_HeightGen(a_HeightGen),
		m_RoadBlock(a_RoadBlock),
		m_WaterRoadBlock(a_WaterRoadBlock)
	{
		// Generate the pieces for this village; don't care about the Y coord:
		cBFSPieceGenerator pg(*this, a_Seed);
		pg.PlacePieces(a_OriginX, 0, a_OriginZ, a_MaxRoadDepth + 1, m_Pieces);
		if (m_Pieces.empty())
		{
			return;
		}
		
		// If the central piece should be moved to ground, move it, and
		// check all of its dependents and move those that are strictly connector-driven based on its new Y coord:
		if (((cPrefab &)m_Pieces[0]->GetPiece()).ShouldMoveToGround())
		{
			int OrigPosY = m_Pieces[0]->GetCoords().y;
			PlacePieceOnGround(*m_Pieces[0]);
			int NewPosY = m_Pieces[0]->GetCoords().y;
			MoveAllDescendants(m_Pieces, 0, NewPosY - OrigPosY);
		}
	}
	
	~cVillage()
	{
		cPieceGenerator::FreePieces(m_Pieces);
	}
	
protected:
	/** Seed for the random functions */
	int m_Seed;
	
	/** The noise used as a pseudo-random generator */
	cNoise m_Noise;
	
	/** Maximum size, in X/Z blocks, of the village (radius from the origin) */
	int m_MaxSize;
	
	/** The density for this village. Used to refrain from populating all house connectors. Range [0, 100] */
	int m_Density;
	
	/** Borders of the village - no item may reach out of this cuboid. */
	cCuboid m_Borders;
	
	/** Prefabs to use for buildings */
	cPiecePool & m_Prefabs;
	
	/** The underlying height generator, used for placing the structures on top of the terrain. */
	cTerrainHeightGenPtr m_HeightGen;
	
	/** The village pieces, placed by the generator. */
	cPlacedPieces m_Pieces;
	
	/** The block to use for the roads. */
	BLOCKTYPE m_RoadBlock;

	/** The block used for the roads if the road is on water. */
	BLOCKTYPE m_WaterRoadBlock;
	
	
	// cGridStructGen::cStructure overrides:
	virtual void DrawIntoChunk(cChunkDesc & a_Chunk) override
	{
		// Iterate over all items
		// Each intersecting prefab is placed on ground, then drawn
		// Each intersecting road is drawn by replacing top soil blocks with gravel / sandstone blocks
		cChunkDef::HeightMap HeightMap;  // Heightmap for this chunk, used by roads
		m_HeightGen->GenHeightMap(a_Chunk.GetChunkX(), a_Chunk.GetChunkZ(), HeightMap);
		for (cPlacedPieces::iterator itr = m_Pieces.begin(), end = m_Pieces.end(); itr != end; ++itr)
		{
			cPrefab & Prefab = (cPrefab &)((*itr)->GetPiece());
			if ((*itr)->GetPiece().GetSize().y == 1)
			{
				// It's a road, special handling (change top terrain blocks to m_RoadBlock)
				DrawRoad(a_Chunk, **itr, HeightMap);
				continue;
			}
			if (Prefab.ShouldMoveToGround() && !(*itr)->HasBeenMovedToGround())
			{
				PlacePieceOnGround(**itr);
			}
			Prefab.Draw(a_Chunk, *itr);
		}  // for itr - m_PlacedPieces[]
	}
	
	
	/**  Adjusts the Y coord of the given piece so that the piece is on the ground.
	Ground level is assumed to be represented by the first connector in the piece. */
	void PlacePieceOnGround(cPlacedPiece & a_Piece)
	{
		cPiece::cConnector FirstConnector = a_Piece.GetRotatedConnector(0);
		int ChunkX, ChunkZ;
		int BlockX = FirstConnector.m_Pos.x;
		int BlockZ = FirstConnector.m_Pos.z;
		int BlockY;
		cChunkDef::AbsoluteToRelative(BlockX, BlockY, BlockZ, ChunkX, ChunkZ);
		cChunkDef::HeightMap HeightMap;
		m_HeightGen->GenHeightMap(ChunkX, ChunkZ, HeightMap);
		int TerrainHeight = cChunkDef::GetHeight(HeightMap, BlockX, BlockZ);
		a_Piece.MoveToGroundBy(TerrainHeight - FirstConnector.m_Pos.y + 1);
	}
	
	
	/** Draws the road into the chunk.
	The heightmap is not queried from the heightgen, but is given via parameter, so that it may be queried just
	once for all roads in a chunk. */
	void DrawRoad(cChunkDesc & a_Chunk, cPlacedPiece & a_Road, cChunkDef::HeightMap & a_HeightMap)
	{
		cCuboid RoadCoords = a_Road.GetHitBox();
		RoadCoords.Sort();
		int MinX = std::max(RoadCoords.p1.x - a_Chunk.GetChunkX() * cChunkDef::Width, 0);
		int MaxX = std::min(RoadCoords.p2.x - a_Chunk.GetChunkX() * cChunkDef::Width, cChunkDef::Width - 1);
		int MinZ = std::max(RoadCoords.p1.z - a_Chunk.GetChunkZ() * cChunkDef::Width, 0);
		int MaxZ = std::min(RoadCoords.p2.z - a_Chunk.GetChunkZ() * cChunkDef::Width, cChunkDef::Width - 1);
		for (int z = MinZ; z <= MaxZ; z++)
		{
			for (int x = MinX; x <= MaxX; x++)
			{
				if (IsBlockWater(a_Chunk.GetBlockType(x, cChunkDef::GetHeight(a_HeightMap, x, z), z)))
				{
					a_Chunk.SetBlockType(x, cChunkDef::GetHeight(a_HeightMap, x, z), z, m_WaterRoadBlock);
				}
				else
				{
					a_Chunk.SetBlockType(x, cChunkDef::GetHeight(a_HeightMap, x, z), z, m_RoadBlock);
				}
			}
		}
	}
	
	
	// cPiecePool overrides:
	virtual cPieces GetPiecesWithConnector(int a_ConnectorType)
	{
		return m_Prefabs.GetPiecesWithConnector(a_ConnectorType);
	}
	
	
	virtual cPieces GetStartingPieces(void)
	{
		return m_Prefabs.GetStartingPieces();
	}
	
	
	virtual int GetPieceWeight(
		const cPlacedPiece & a_PlacedPiece,
		const cPiece::cConnector & a_ExistingConnector,
		const cPiece & a_NewPiece
	) override
	{
		// Check against the density:
		if (a_ExistingConnector.m_Type == 1)
		{
			const Vector3i & Coords = a_PlacedPiece.GetRotatedConnector(a_ExistingConnector).m_Pos;
			int rnd = (m_Noise.IntNoise3DInt(Coords.x, Coords.y, Coords.z) / 7) % 100;
			if (rnd > m_Density)
			{
				return 0;
			}
		}
		
		// Density check passed, relay to m_Prefabs:
		return m_Prefabs.GetPieceWeight(a_PlacedPiece, a_ExistingConnector, a_NewPiece);
	}
	
	
	virtual int GetStartingPieceWeight(const cPiece & a_NewPiece) override
	{
		return m_Prefabs.GetStartingPieceWeight(a_NewPiece);
	}
	
	
	virtual void PiecePlaced(const cPiece & a_Piece) override
	{
		m_Prefabs.PiecePlaced(a_Piece);
	}
	
	
	virtual void Reset(void) override
	{
		m_Prefabs.Reset();
	}
	
	
	void MoveAllDescendants(cPlacedPieces & a_PlacedPieces, size_t a_Pivot, int a_HeightDifference)
	{
		size_t num = a_PlacedPieces.size();
		cPlacedPiece * Pivot = a_PlacedPieces[a_Pivot];
		for (size_t i = a_Pivot + 1; i < num; i++)
		{
			if (
				(a_PlacedPieces[i]->GetParent() == Pivot) &&  // It is a direct dependant of the pivot
				!((const cPrefab &)a_PlacedPieces[i]->GetPiece()).ShouldMoveToGround()  // It attaches strictly by connectors
			)
			{
				a_PlacedPieces[i]->MoveToGroundBy(a_HeightDifference);
				MoveAllDescendants(a_PlacedPieces, i, a_HeightDifference);
			}
		}  // for i - a_PlacedPieces[]
	}
} ;





////////////////////////////////////////////////////////////////////////////////
// cVillageGen:

static cVillagePiecePool g_SandVillage(g_SandVillagePrefabs, g_SandVillagePrefabsCount, g_SandVillageStartingPrefabs, g_SandVillageStartingPrefabsCount);
static cVillagePiecePool g_SandFlatRoofVillage(g_SandFlatRoofVillagePrefabs, g_SandFlatRoofVillagePrefabsCount, g_SandFlatRoofVillageStartingPrefabs, g_SandFlatRoofVillageStartingPrefabsCount);
static cVillagePiecePool g_AlchemistVillage(g_AlchemistVillagePrefabs, g_AlchemistVillagePrefabsCount, g_AlchemistVillageStartingPrefabs, g_AlchemistVillageStartingPrefabsCount);
static cVillagePiecePool g_PlainsVillage(g_PlainsVillagePrefabs, g_PlainsVillagePrefabsCount, g_PlainsVillageStartingPrefabs, g_PlainsVillageStartingPrefabsCount);
static cVillagePiecePool g_JapaneseVillage(g_JapaneseVillagePrefabs, g_JapaneseVillagePrefabsCount, g_JapaneseVillageStartingPrefabs, g_JapaneseVillageStartingPrefabsCount);

static cVillagePiecePool * g_DesertVillagePools[] =
{
	&g_SandVillage,
	&g_SandFlatRoofVillage,
	&g_AlchemistVillage,
} ;

static cVillagePiecePool * g_PlainsVillagePools[] =
{
	&g_PlainsVillage,
	&g_JapaneseVillage,
} ;





cVillageGen::cVillageGen(int a_Seed, int a_GridSize, int a_MaxOffset, int a_MaxDepth, int a_MaxSize, int a_MinDensity, int a_MaxDensity, cBiomeGenPtr a_BiomeGen, cTerrainHeightGenPtr a_HeightGen) :
	super(a_Seed, a_GridSize, a_GridSize, a_MaxOffset, a_MaxOffset, a_MaxSize, a_MaxSize, 100),
	m_Noise(a_Seed + 1000),
	m_MaxDepth(a_MaxDepth),
	m_MaxSize(a_MaxSize),
	m_MinDensity(a_MinDensity),
	m_MaxDensity(a_MaxDensity),
	m_BiomeGen(a_BiomeGen),
	m_HeightGen(a_HeightGen)
{
}





cGridStructGen::cStructurePtr cVillageGen::CreateStructure(int a_GridX, int a_GridZ, int a_OriginX, int a_OriginZ)
{
	// Generate the biomes for the chunk surrounding the origin:
	int ChunkX, ChunkZ;
	cChunkDef::BlockToChunk(a_OriginX, a_OriginZ, ChunkX, ChunkZ);
	cChunkDef::BiomeMap Biomes;
	m_BiomeGen->GenBiomes(ChunkX, ChunkZ, Biomes);

	// Check if all the biomes are village-friendly:
	// If just one is not, no village is created, because it's likely that an unfriendly biome is too close
	cVillagePiecePool * VillagePrefabs = nullptr;
	BLOCKTYPE RoadBlock = E_BLOCK_GRAVEL;
	BLOCKTYPE WaterRoadBlock = E_BLOCK_PLANKS;
	int rnd = m_Noise.IntNoise2DInt(a_OriginX, a_OriginZ) / 11;
	cVillagePiecePool * PlainsVillage = g_PlainsVillagePools[rnd % ARRAYCOUNT(g_PlainsVillagePools)];
	cVillagePiecePool * DesertVillage = g_DesertVillagePools[rnd % ARRAYCOUNT(g_DesertVillagePools)];
	for (size_t i = 0; i < ARRAYCOUNT(Biomes); i++)
	{
		switch (Biomes[i])
		{
			case biDesert:
			case biDesertM:
			{
				// These biomes allow sand villages
				VillagePrefabs = DesertVillage;
				// RoadBlock = E_BLOCK_SANDSTONE;
				break;
			}
			case biPlains:
			case biSavanna:
			case biSavannaM:
			case biSunflowerPlains:
			{
				// These biomes allow plains-style villages
				VillagePrefabs = PlainsVillage;
				break;
			}
			default:
			{
				// Village-unfriendly biome, bail out with zero structure:
				return cStructurePtr();
			}
		}  // switch (Biomes[i])
	}  // for i - Biomes[]

	// Choose density for the village, random between m_MinDensity and m_MaxDensity:
	int Density;
	if (m_MaxDensity > m_MinDensity)
	{
		Density = m_MinDensity + rnd % (m_MaxDensity - m_MinDensity);
	}
	else
	{
		Density = m_MinDensity;
	}
	
	// Create a village based on the chosen prefabs:
	if (VillagePrefabs == nullptr)
	{
		return cStructurePtr();
	}
	return cStructurePtr(new cVillage(m_Seed, a_GridX, a_GridZ, a_OriginX, a_OriginZ, m_MaxDepth, m_MaxSize, Density, *VillagePrefabs, m_HeightGen, RoadBlock, WaterRoadBlock));
}




