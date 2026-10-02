///----------------------------------------------------------------------------------------------------
/// Copyright (c) Raidcore.GG - Licensed under the MIT license.
///
/// Name         :  Mumble.h
/// Description  :  Mumble header definitions.
/// Authors      :  K. Bieniek
///----------------------------------------------------------------------------------------------------

#pragma once

#include <cstdint>

namespace Mumble
{
	struct Vector2
	{
		float X;
		float Y;
	};

	struct Vector3
	{
		float X;
		float Y;
		float Z;
	};

	/* enums */
	enum class EMapType : uint8_t
	{
		AutoRedirect,
		CharacterCreation,
		PvP,
		GvG,
		Instance,
		Public,
		Tournament,
		Tutorial,
		UserTournament,
		WvW_EternalBattlegrounds,
		WvW_BlueBorderlands,
		WvW_GreenBorderlands,
		WvW_RedBorderlands,
		WVW_FortunesVale,
		WvW_ObsidianSanctum,
		WvW_EdgeOfTheMists,
		Public_Mini,
		BigBattle,
		WvW_Lounge
	};

	enum class EMountIndex : uint8_t
	{
		None,
		Jackal,
		Griffon,
		Springer,
		Skimmer,
		Raptor,
		RollerBeetle,
		Warclaw,
		Skyscale,
		Skiff,
		SiegeTurtle
	};

	enum class EProfession : uint8_t
	{
		None,
		Guardian,
		Warrior,
		Engineer,
		Ranger,
		Thief,
		Elementalist,
		Mesmer,
		Necromancer,
		Revenant
	};

	enum class ERace : uint8_t
	{
		Asura,
		Charr,
		Human,
		Norn,
		Sylvari
	};

	enum class EUIScale : uint8_t
	{
		Small,
		Normal,
		Large,
		Larger
	};

	/* structs */
	struct Identity
	{
		char        Name[20];
		EProfession Profession;
		uint32_t    Specialization;
		ERace       Race;
		uint32_t    MapID;
		uint32_t    WorldID;
		uint32_t    TeamColorID;
		bool        IsCommander; // is the player currently tagged up
		float       FOV;
		EUIScale    UISize;
	};

	struct Compass
	{
		uint16_t Width;
		uint16_t Height;
		float    Rotation;       // radians
		Vector2  PlayerPosition; // continent
		Vector2  Center;         // continent
		float    Scale;
	};

	struct Context
	{
		uint8_t     ServerAddress[28]; // union sockaddr_in, sockaddr_in6
		uint32_t    MapID;
		EMapType    MapType;
		uint32_t    ShardID;
		uint32_t    InstanceID;
		uint32_t    BuildID;
		uint32_t    IsMapOpen         : 1;
		uint32_t    IsCompassTopRight : 1;
		uint32_t    IsCompassRotating : 1;
		uint32_t    IsGameFocused     : 1;
		uint32_t    IsCompetitive     : 1;
		uint32_t    IsTextboxFocused  : 1;
		uint32_t    IsInCombat        : 1;
		Compass     Compass;
		uint32_t    ProcessID;
		EMountIndex MountIndex;
	};

	struct Data
	{
		uint32_t UIVersion;
		uint32_t UITick;
		Vector3  AvatarPosition;
		Vector3  AvatarFront;
		Vector3  AvatarTop;
		wchar_t  Name[256];
		Vector3  CameraPosition;
		Vector3  CameraFront;
		Vector3  CameraTop;
		wchar_t  Identity[256];
		uint32_t ContextLength; // always 48, not the actual length of the context data
		Context  Context;
		wchar_t  Description[2048];
	};
}
