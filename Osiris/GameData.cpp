#include <atomic>
#include <cstring>
#include <list>
#include <mutex>

#include "Config.h"
#include "fnv.h"
#include "GameData.h"
#include "Interfaces.h"
#include "Memory.h"

#include "Resources/avatar_ct.h"
#include "Resources/avatar_tt.h"
#include "Resources/skillgroups.h"

#include "stb_image.h"

#include "SDK/ClientClass.h"
#include "SDK/Engine.h"
#include "SDK/EngineTrace.h"
#include "SDK/Entity.h"
#include "SDK/EntityList.h"
#include "SDK/GlobalVars.h"
#include "SDK/Localize.h"
#include "SDK/LocalPlayer.h"
#include "SDK/ModelInfo.h"
#include "SDK/NetworkChannel.h"
#include "SDK/PlayerResource.h"
#include "SDK/Sound.h"
#include "SDK/Steam.h"
#include "SDK/WeaponId.h"
#include "SDK/WeaponData.h"

static Matrix4x4 viewMatrix;
static LocalPlayerData localPlayerData;
static std::vector<PlayerData> playerData;
static std::vector<ObserverData> observerData;
static std::vector<WeaponData> weaponData;
static std::vector<EntityData> entityData;
static std::vector<LootCrateData> lootCrateData;
static std::forward_list<ProjectileData> projectileData;
static BombData bombData;
static std::vector<InfernoData> infernoData;
static std::vector<SmokeData> smokeData;
static std::atomic_int netOutgoingLatency;
static std::string gameModeName;
static std::array<std::string, 19> skillGroupNames;
static std::array<std::string, 16> skillGroupNamesDangerzone;


static auto playerByHandleWritable(int handle) noexcept
{
    const auto it = std::ranges::find(playerData, handle, &PlayerData::handle);
    return it != playerData.end() ? &(*it) : nullptr;
}

static void updateNetLatency() noexcept
{
    if (const auto networkChannel = interfaces->engine->getNetworkChannel())
        netOutgoingLatency = (std::max)(static_cast<int>(networkChannel->getLatency(0) * 1000.0f), 0);
    else
        netOutgoingLatency = 0;
}

constexpr auto playerVisibilityUpdateDelay = 0.1f;
static float nextPlayerVisibilityUpdateTime = 0.0f;

static bool shouldUpdatePlayerVisibility() noexcept
{
    return nextPlayerVisibilityUpdateTime <= memory->globalVars->realtime;
}

void GameData::update() noexcept
{
    static int lastFrame;
    if (lastFrame == memory->globalVars->framecount)
        return;
    lastFrame = memory->globalVars->framecount;

    updateNetLatency();

    Lock lock;
    observerData.clear();
    weaponData.clear();
    entityData.clear();
    lootCrateData.clear();
    infernoData.clear();
    smokeData.clear();

    localPlayerData.update();
    bombData.update();

    if (static bool skillgroupNamesInitialized = false; !skillgroupNamesInitialized) {
        for (std::size_t i = 0; i < skillGroupNames.size(); ++i)
        {
            const auto rank = interfaces->localize->findAsUTF8(("RankName_" + std::to_string(i)).c_str());
            skillGroupNames[i] = std::string(rank);
        }

        for (std::size_t i = 0; i < skillGroupNamesDangerzone.size(); ++i)
        {
            const auto rank = interfaces->localize->findAsUTF8(("skillgroup_" + std::to_string(i) + "dangerzone").c_str());
            skillGroupNamesDangerzone[i] = std::string(rank);
        }

        skillgroupNamesInitialized = true;
    }

    if (!localPlayer) {
        playerData.clear();
        projectileData.clear();
        gameModeName.clear();
        return;
    }

    gameModeName = memory->getGameModeName(false);
    viewMatrix = interfaces->engine->worldToScreenMatrix();

    const auto observerTarget = localPlayer->getObserverMode() == ObsMode::InEye ? localPlayer->getObserverTarget() : nullptr;

    const auto highestEntityIndex = interfaces->entityList->getHighestEntityIndex();
    for (int i = 1; i <= highestEntityIndex; ++i) {
        const auto entity = interfaces->entityList->getEntity(i);
        if (!entity)
            continue;

        if (entity->isPlayer()) {
            if (entity == localPlayer.get() || entity == observerTarget)
                continue;

            if (const auto player = playerByHandleWritable(entity->handle())) {
                player->update(entity);
            } else {
                playerData.emplace_back(entity);
            }

            if (!entity->isDormant() && !entity->isAlive()) {
                if (const auto obs = entity->getObserverTarget())
                    observerData.emplace_back(entity, obs, obs == localPlayer.get());
            }
        } else {
            if (entity->isDormant())
                continue;

            if (entity->isWeapon()) {
                if (entity->ownerEntity() == -1)
                    weaponData.emplace_back(entity);
            } else {
                const auto classId = entity->getClientClass()->classId;
                const int classIdInt = (int)classId;
                switch (classId) {
                case ClassId::BaseCSGrenadeProjectile:
                    if (!entity->shouldDraw()) {
                        if (const auto it = std::find(projectileData.begin(), projectileData.end(), entity->handle()); it != projectileData.end())
                            it->exploded = true;
                        break;
                    }
                    [[fallthrough]];
                case ClassId::BreachChargeProjectile:
                case ClassId::BumpMineProjectile:
                case ClassId::DecoyProjectile:
                case ClassId::MolotovProjectile:
                case ClassId::SensorGrenadeProjectile:
                case ClassId::SmokeGrenadeProjectile:
                case ClassId::SnowballProjectile:
                    if (const auto it = std::find(projectileData.begin(), projectileData.end(), entity->handle()); it != projectileData.end())
                        it->update(entity);
                    else
                        projectileData.emplace_front(entity);
                    break;
                case ClassId::DynamicProp:
                    if (const auto model = entity->getModel(); !model || !std::strstr(model->name, "challenge_coin"))
                        break;
                    [[fallthrough]];
                case ClassId::EconEntity:
                case ClassId::Chicken:
                case ClassId::PlantedC4:
                case ClassId::Hostage:
                case ClassId::Dronegun:
                case ClassId::Cash:
                case ClassId::AmmoBox:
                case ClassId::RadarJammer:
                case ClassId::SnowballPile:
                    entityData.emplace_back(entity);
                    break;
                case ClassId::LootCrate:
                    lootCrateData.emplace_back(entity);
                    break;
                case ClassId::Inferno:
                    infernoData.emplace_back(entity);
                    break;
                }

                if (classIdInt == dynamicClassId->fogController && !config->visuals.noFog)
                {
                    const auto fog = reinterpret_cast<FogController*>(entity);

                    unsigned char _color[3];

                    if (config->visuals.fog.rainbow)
                    {
                        const auto [colorR, colorG, colorB] { rainbowColor(config->visuals.fog.rainbowSpeed) };
                        _color[0] = std::clamp(static_cast<int>(colorR * 255.0f), 0, 255);
                        _color[1] = std::clamp(static_cast<int>(colorG * 255.0f), 0, 255);
                        _color[2] = std::clamp(static_cast<int>(colorB * 255.0f), 0, 255);
                    }
                    else
                    {
                        _color[0] = std::clamp(static_cast<int>(config->visuals.fog.color[0] * 255.0f), 0, 255);
                        _color[1] = std::clamp(static_cast<int>(config->visuals.fog.color[1] * 255.0f), 0, 255);
                        _color[2] = std::clamp(static_cast<int>(config->visuals.fog.color[2] * 255.0f), 0, 255);
                    }

                    const unsigned long color = *(unsigned long*)_color;

                    fog->enable() = config->visuals.fog.enabled ? 1 : 0;
                    fog->start() = config->visuals.fogOptions.start;
                    fog->end() = config->visuals.fogOptions.end;
                    fog->density() = config->visuals.fogOptions.density;
                    fog->color() = color;
                }

                if (classId == ClassId::SmokeGrenadeProjectile && entity->didSmokeEffect())
                    smokeData.emplace_back(entity);
            }
        }
    }

    std::sort(playerData.begin(), playerData.end());
    std::sort(weaponData.begin(), weaponData.end());
    std::sort(entityData.begin(), entityData.end());
    std::sort(lootCrateData.begin(), lootCrateData.end());

    std::for_each(projectileData.begin(), projectileData.end(), [](auto& projectile) {
        if (interfaces->entityList->getEntityFromHandle(projectile.handle) == nullptr)
            projectile.exploded = true;
    });

    std::erase_if(projectileData, [](const auto& projectile) { return interfaces->entityList->getEntityFromHandle(projectile.handle) == nullptr
        && (projectile.trajectory.size() < 1 || projectile.trajectory[projectile.trajectory.size() - 1].first + 60.0f < memory->globalVars->realtime); });

    std::erase_if(playerData, [](const auto& player) { return interfaces->entityList->getEntityFromHandle(player.handle) == nullptr; });

    if (shouldUpdatePlayerVisibility())
        nextPlayerVisibilityUpdateTime = memory->globalVars->realtime + playerVisibilityUpdateDelay;
}

void GameData::clearProjectileList() noexcept
{
    Lock lock;
    projectileData.clear();
}

static void clearSkillgroupTextures() noexcept;
static void clearAvatarTextures() noexcept;

struct PlayerAvatar {
    mutable Texture texture;
    std::unique_ptr<std::uint8_t[]> rgba;
};

static std::unordered_map<int, PlayerAvatar> playerAvatars;

void GameData::clearTextures() noexcept
{
    Lock lock;

    clearSkillgroupTextures();
    clearAvatarTextures();
    for (const auto& [handle, avatar] : playerAvatars)
        avatar.texture.clear();
}

void GameData::clearUnusedAvatars() noexcept
{
    Lock lock;
    std::erase_if(playerAvatars, [](const auto& pair) { return std::ranges::find(std::as_const(playerData), pair.first, &PlayerData::handle) == playerData.cend(); });
}

int GameData::getNetOutgoingLatency() noexcept
{
    return netOutgoingLatency;
}

const Matrix4x4& GameData::toScreenMatrix() noexcept
{
    return viewMatrix;
}

const LocalPlayerData& GameData::local() noexcept
{
    return localPlayerData;
}

const std::vector<PlayerData>& GameData::players() noexcept
{
    return playerData;
}

const PlayerData* GameData::playerByHandle(int handle) noexcept
{
    return playerByHandleWritable(handle);
}

const std::vector<ObserverData>& GameData::observers() noexcept
{
    return observerData;
}

const std::vector<WeaponData>& GameData::weapons() noexcept
{
    return weaponData;
}

const std::vector<EntityData>& GameData::entities() noexcept
{
    return entityData;
}

const std::vector<LootCrateData>& GameData::lootCrates() noexcept
{
    return lootCrateData;
}

const std::forward_list<ProjectileData>& GameData::projectiles() noexcept
{
    return projectileData;
}

const BombData& GameData::plantedC4() noexcept
{
    return bombData;
}

const std::string& GameData::gameMode() noexcept
{
    return gameModeName;
}

const std::vector<InfernoData>& GameData::infernos() noexcept
{
    return infernoData;
}

const std::vector<SmokeData>& GameData::smokes() noexcept
{
    return smokeData;
}

void LocalPlayerData::update() noexcept
{
    if (!localPlayer) {
        exists = false;
        return;
    }

    exists = true;
    alive = localPlayer->isAlive();
    inaccuracy = Vector{};
    team = localPlayer->getTeamNumber();
    velocityModifier = localPlayer->velocityModifier();

    if (const auto activeWeapon = localPlayer->getActiveWeapon()) {
        inaccuracy = localPlayer->getEyePosition() + Vector::fromAngle(interfaces->engine->getViewAngles() + Vector{ Helpers::rad2deg(activeWeapon->getInaccuracy() + activeWeapon->getSpread()), 0.0f, 0.0f }) * 1000.0f;
        inReload = activeWeapon->isInReload();
        noScope = activeWeapon->isSniperRifle() && !localPlayer->isScoped();
        nextWeaponAttack = activeWeapon->nextPrimaryAttack();
        shooting = activeWeapon->isPistol() ? !inReload && nextWeaponAttack > memory->globalVars->serverTime() : localPlayer->shotsFired() > 1;
    }
    fov = localPlayer->fov() ? localPlayer->fov() : localPlayer->defaultFov();
    handle = localPlayer->handle();
    flashDuration = localPlayer->flashDuration();

    aimPunch = localPlayer->getEyePosition() + Vector::fromAngle(interfaces->engine->getViewAngles() + localPlayer->getAimPunch()) * 1000.0f;

    const auto obsMode = localPlayer->getObserverMode();
    if (const auto obs = localPlayer->getObserverTarget(); obs && obsMode != ObsMode::Roaming && obsMode != ObsMode::Deathcam)
        origin = obs->getAbsOrigin();
    else
        origin = localPlayer->getAbsOrigin();
}

BaseData::BaseData(Entity* entity) noexcept
{
    distanceToLocal = entity->getAbsOrigin().distTo(localPlayerData.origin);
 
    if (entity->isPlayer()) {
        const auto collideable = entity->getCollideable();
        obbMins = collideable->obbMins();
        obbMaxs = collideable->obbMaxs();
    } else if (const auto model = entity->getModel()) {
        obbMins = model->mins;
        obbMaxs = model->maxs;
    }

    coordinateFrame = entity->toWorldTransform();
}

EntityData::EntityData(Entity* entity) noexcept : BaseData{ entity }
{
    name = [](Entity* entity) {
        switch (entity->getClientClass()->classId) {
        case ClassId::EconEntity: return "Defuse Kit";
        case ClassId::Chicken: return "Chicken";
        case ClassId::PlantedC4: return "Planted C4";
        case ClassId::Hostage: return "Hostage";
        case ClassId::Dronegun: return "Sentry";
        case ClassId::Cash: return "Cash";
        case ClassId::AmmoBox: return "Ammo Box";
        case ClassId::RadarJammer: return "Radar Jammer";
        case ClassId::SnowballPile: return "Snowball Pile";
        case ClassId::DynamicProp: return "Collectable Coin";
        default: assert(false); return "unknown";
        }
    }(entity);
}

ProjectileData::ProjectileData(Entity* projectile) noexcept : BaseData { projectile }
{
    name = [](Entity* projectile) {
        switch (projectile->getClientClass()->classId) {
        case ClassId::BaseCSGrenadeProjectile:
            if (const auto model = projectile->getModel(); model && strstr(model->name, "flashbang"))
                return "Flashbang";
            else
                return "HE Grenade";
        case ClassId::BreachChargeProjectile: return "Breach Charge";
        case ClassId::BumpMineProjectile: return "Bump Mine";
        case ClassId::DecoyProjectile: return "Decoy Grenade";
        case ClassId::MolotovProjectile: return "Molotov";
        case ClassId::SensorGrenadeProjectile: return "TA Grenade";
        case ClassId::SmokeGrenadeProjectile: return "Smoke Grenade";
        case ClassId::SnowballProjectile: return "Snowball";
        default: assert(false); return "unknown";
        }
    }(projectile);

    if (const auto thrower = interfaces->entityList->getEntityFromHandle(projectile->thrower()); thrower && localPlayer) {
        if (thrower == localPlayer.get())
            thrownByLocalPlayer = true;
        else
            thrownByEnemy = memory->isOtherEnemy(localPlayer.get(), thrower);
    }

    handle = projectile->handle();
}

void ProjectileData::update(Entity* projectile) noexcept
{
    static_cast<BaseData&>(*this) = { projectile };

    if (const auto& pos = projectile->getAbsOrigin(); trajectory.size() < 1 || trajectory[trajectory.size() - 1].second != pos)
        trajectory.emplace_back(memory->globalVars->realtime, pos);
}

PlayerData::PlayerData(Entity* entity) noexcept : BaseData{ entity }, userId{ entity->getUserId() }, steamID{ entity->getSteamId() }, handle{ entity->handle() }, money{ entity->money() }
{
    if (steamID) {
        const auto ctx = interfaces->engine->getSteamAPIContext();
        const auto avatar = ctx->steamFriends->getSmallFriendAvatar(steamID);
        constexpr auto rgbaDataSize = 4 * 32 * 32;

        PlayerAvatar playerAvatar;
        playerAvatar.rgba = std::make_unique<std::uint8_t[]>(rgbaDataSize);
        if (ctx->steamUtils->getImageRGBA(avatar, playerAvatar.rgba.get(), rgbaDataSize))
            playerAvatars[handle] = std::move(playerAvatar);
    }

    update(entity);
}

void PlayerData::update(Entity* entity) noexcept
{
    name = entity->getPlayerName();
    const auto idx = entity->index();

    if (const auto pr = *memory->playerResource) {
        armor = pr->armor()[idx];
        skillgroup = pr->competitiveRanking()[idx];
        competitiveWins = pr->competitiveWins()[idx];
        hasBomb = idx == pr->playerC4Index();
        if (const auto clantag = pr->getClan(idx); 
            clantag && clantag[0] != '\0')
        {
            clanTag = std::string(clantag);
        }
        else
            clanTag = "";
    }

    dormant = entity->isDormant();
    if (dormant) {
        if (const auto pr = *memory->playerResource) {
            alive = pr->getIPlayerResource()->isAlive(idx);
            if (!alive)
                lastContactTime = 0.0f;
            health = pr->getIPlayerResource()->getPlayerHealth(idx);
        }
        return;
    }

    money = entity->money();
    team = entity->getTeamNumber();
    static_cast<BaseData&>(*this) = { entity };
    origin = entity->getAbsOrigin();
    inViewFrustum = !interfaces->engine->cullBox(obbMins + origin, obbMaxs + origin);
    alive = entity->isAlive();
    lastContactTime = alive ? memory->globalVars->realtime : 0.0f;

    const Vector start = entity->getEyePosition();
    const Vector end = start + Vector::fromAngle(entity->eyeAngles()) * 1000.0f;

    Trace trace;
    interfaces->engineTrace->traceRay({ start, end }, 0x80040FF, entity, trace);
    lookingAt = trace.endpos;

    if (localPlayer) {
        enemy = memory->isOtherEnemy(entity, localPlayer.get());

        if (!inViewFrustum || !alive)
            visible = false;
        else if (shouldUpdatePlayerVisibility())
            visible = entity->visibleTo(localPlayer.get());
    }

    constexpr auto isEntityAudible = [](int entityIndex) noexcept {
        for (int i = 0; i < memory->activeChannels->count; ++i)
            if (memory->channels[memory->activeChannels->list[i]].soundSource == entityIndex)
                return true;
        return false;
    };

    audible = isEntityAudible(entity->index());
    spotted = entity->spotted();
    health = entity->health();
    immune = entity->gunGameImmunity();
    flashDuration = entity->flashDuration();

    if (const auto weapon = entity->getActiveWeapon()) {
        audible = audible || isEntityAudible(weapon->index());
        theName = [](WeaponId weaponId) {
            switch (weaponId)
            {
            default: return "All";

            case WeaponId::Glock:     return   "E";
            case WeaponId::Hkp2000:   return   "D";
            case WeaponId::Usp_s:     return   "G";
            case WeaponId::Elite:     return   "B";
            case WeaponId::P250:      return   "F";
            case WeaponId::Tec9:      return   "H";
            case WeaponId::Fiveseven: return   "C";
            case WeaponId::Cz75a:     return   "I";
            case WeaponId::Deagle:    return   "A";
            case WeaponId::Revolver:  return   "J";

            case WeaponId::Mac10: return   "K";
            case WeaponId::Mp9:   return   "O";
            case WeaponId::Mp7:   return   "N";
            case WeaponId::Mp5sd: return   "L";
            case WeaponId::Ump45: return   "L";
            case WeaponId::P90:   return   "P";
            case WeaponId::Bizon: return   "M";

            case WeaponId::GalilAr: return "Q";
            case WeaponId::Famas: return   "R";
            case WeaponId::Ak47: return    "W";
            case WeaponId::M4A1: return    "S";
            case WeaponId::M4a1_s: return  "T";
            case WeaponId::Sg553: return   "V";
            case WeaponId::Aug: return     "U";

            case WeaponId::Ssg08:  return   "a        ";
            case WeaponId::Awp:    return   "Z        ";
            case WeaponId::G3SG1:  return   "X      ";
            case WeaponId::Scar20: return   "Y      ";

            case WeaponId::Nova:     return   "e    ";
            case WeaponId::Xm1014:   return   "b    ";
            case WeaponId::Sawedoff: return   "c    ";
            case WeaponId::Mag7:     return   "d    ";

            case WeaponId::M249:  return   "g";
            case WeaponId::Negev: return   "f";

            case WeaponId::Flashbang:    return   "i";
            case WeaponId::HeGrenade:    return   "j";
            case WeaponId::SmokeGrenade: return   "k";
            case WeaponId::Molotov:      return   "l";
            case WeaponId::Decoy:        return   "m";
            case WeaponId::IncGrenade:   return   "l";
            case WeaponId::TaGrenade:    return   "TA Grenade";
            case WeaponId::Firebomb:     return   "Fire Bomb";
            case WeaponId::Diversion:    return   "Diversion";
            case WeaponId::FragGrenade:  return   "Frag Grenade";
            case WeaponId::Snowball:     return   "Snowball";

            case WeaponId::Axe:     return   "Axe";
            case WeaponId::Hammer:  return   "Hammer";
            case WeaponId::Spanner: return   "Wrench";

            case WeaponId::C4:           return   "o";
            case WeaponId::Healthshot:   return   "Healthshot";
            case WeaponId::BumpMine:     return   "Bump Mine";
            case WeaponId::ZoneRepulsor: return   "Zone Repulsor";
            case WeaponId::Shield:       return   "Shield";

            case WeaponId::Bayonet:       return   "1   ";
            case WeaponId::Flip:          return   "2   ";
            case WeaponId::Gut:           return   "3   ";
            case WeaponId::Karambit:      return   "4   ";
            case WeaponId::M9Bayonet:     return   "5   ";
            case WeaponId::Huntsman:      return   "6   ";
            case WeaponId::Bowie:         return   "7   ";
            case WeaponId::Butterfly:     return   "8   ";
            case WeaponId::Daggers:       return   "9   ";
            case WeaponId::Falchion:      return   "10   ";
            case WeaponId::ClassicKnife:  return   "1   ";
            case WeaponId::Knife:         return   "1   ";
            case WeaponId::KnifeT:        return   "1   ";
            case WeaponId::GhostKnife:    return   "1   ";
            case WeaponId::GoldenKnife:   return   "1   ";
            case WeaponId::NomadKnife:    return   "1   ";
            case WeaponId::SkeletonKnife: return   "1   ";
            case WeaponId::SurvivalKnife: return   "1   ";
            case WeaponId::Ursus:         return   "1   ";
            case WeaponId::Stiletto:      return   "1   ";
            case WeaponId::Talon:         return   "1   ";
            case WeaponId::Paracord:      return   "1   ";
            case WeaponId::Navaja:        return   "1   ";

            case WeaponId::Taser:         return "h";
            }
        }(weapon->itemDefinitionIndex2());


        activeWeaponIcon = interfaces->localize->findAsUTF8(theName);
       
        const auto weaponInfo = weapon->getWeaponData();
        ammoInClip = weapon->clip();
        MaxAmmo = weaponInfo->maxClip;
        if (const auto weaponInfo2 = weapon->getWeaponData()) {
            activeWeapon = interfaces->localize->findAsUTF8(weaponInfo2->name);
        }

    }
    if (!alive || !inViewFrustum)
        return;

    const auto model = entity->getModel();
    if (!model)
        return;

    const auto studioModel = interfaces->modelInfo->getStudioModel(model);
    if (!studioModel)
        return;

    if (!entity->getBoneCache().memory)
        return;

    matrix3x4 boneMatrices[MAXSTUDIOBONES];
    memcpy(boneMatrices, entity->getBoneCache().memory, std::clamp(entity->getBoneCache().size, 0, MAXSTUDIOBONES) * sizeof(matrix3x4));

    bones.clear();
    bones.reserve(20);

    for (int i = 0; i < studioModel->numBones; ++i) {
        const auto bone = studioModel->getBone(i);

        if (!bone || bone->parent == -1 || !(bone->flags & BONE_USED_BY_HITBOX))
            continue;

        bones.emplace_back(boneMatrices[i].origin(), boneMatrices[bone->parent].origin());
    }

    const auto set = studioModel->getHitboxSet(entity->hitboxSet());
    if (!set)
        return;

    const auto headBox = set->getHitbox(0);

    headMins = headBox->bbMin.transform(boneMatrices[headBox->bone]);
    headMaxs = headBox->bbMax.transform(boneMatrices[headBox->bone]);

    if (headBox->capsuleRadius > 0.0f) {
        headMins -= headBox->capsuleRadius;
        headMaxs += headBox->capsuleRadius;
    }
}

const std::string PlayerData::getRankName() const noexcept
{
    if (gameModeName == "survival")
        return skillGroupNamesDangerzone[std::size_t(skillgroup) < skillGroupNamesDangerzone.size() ? skillgroup : 0];
    else
        return skillGroupNames[std::size_t(skillgroup) < skillGroupNames.size() ? skillgroup : 0];
}

struct PNGTexture {
    template <std::size_t N>
    PNGTexture(const std::array<char, N>& png) noexcept : pngData{ png.data() }, pngDataSize{ png.size() } {}

    ImTextureID getTexture() const noexcept
    {
        if (!texture.get()) {
            int width, height;
            stbi_set_flip_vertically_on_load_thread(false);

            if (const auto data = stbi_load_from_memory((const stbi_uc*)pngData, pngDataSize, &width, &height, nullptr, STBI_rgb_alpha)) {
                texture.init(width, height, data);
                stbi_image_free(data);
            } else {
                assert(false);
            }
        }

        return texture.get();
    }

    void clearTexture() const noexcept { texture.clear(); }

private:
    const char* pngData;
    std::size_t pngDataSize;

    mutable Texture texture;
};

static const auto skillgroupImages = std::array<PNGTexture, 19>({
Resource::skillgroup0, Resource::skillgroup1, Resource::skillgroup2, Resource::skillgroup3, Resource::skillgroup4, Resource::skillgroup5, Resource::skillgroup6, Resource::skillgroup7,
Resource::skillgroup8, Resource::skillgroup9, Resource::skillgroup10, Resource::skillgroup11, Resource::skillgroup12, Resource::skillgroup13, Resource::skillgroup14, Resource::skillgroup15,
Resource::skillgroup16, Resource::skillgroup17, Resource::skillgroup18 });

static const auto dangerZoneImages = std::array<PNGTexture, 16>({
Resource::dangerzone0, Resource::dangerzone1, Resource::dangerzone2, Resource::dangerzone3, Resource::dangerzone4, Resource::dangerzone5, Resource::dangerzone6, Resource::dangerzone7,
Resource::dangerzone8, Resource::dangerzone9, Resource::dangerzone10, Resource::dangerzone11, Resource::dangerzone12, Resource::dangerzone13, Resource::dangerzone14, Resource::dangerzone15 });


static const PNGTexture avatarTT{ Resource::avatar_tt };
static const PNGTexture avatarCT{ Resource::avatar_ct };

static void clearAvatarTextures() noexcept
{
    avatarTT.clearTexture();
    avatarCT.clearTexture();
}

ImTextureID PlayerData::getAvatarTexture() const noexcept
{
    const auto it = std::as_const(playerAvatars).find(handle);
    if (it == playerAvatars.cend())
        return team == Team::TT ? avatarTT.getTexture() : avatarCT.getTexture();

    const auto& avatar = it->second;
    if (!avatar.texture.get())
        avatar.texture.init(32, 32, avatar.rgba.get());
    return avatar.texture.get();
}

static void clearSkillgroupTextures() noexcept
{
    for (const auto& img : skillgroupImages)
        img.clearTexture();
    for (const auto& img : dangerZoneImages)
        img.clearTexture();
}

ImTextureID PlayerData::getRankTexture() const noexcept
{
    if (gameModeName == "survival")
        return dangerZoneImages[std::size_t(skillgroup) < dangerZoneImages.size() ? skillgroup : 0].getTexture();
    else
        return skillgroupImages[std::size_t(skillgroup) < skillgroupImages.size() ? skillgroup : 0].getTexture();
}

float PlayerData::fadingAlpha() const noexcept
{
    constexpr float fadeTime = 1.50f;
    return std::clamp(1.0f - (memory->globalVars->realtime - lastContactTime - 0.25f) / fadeTime, 0.0f, 1.0f);
}

WeaponData::WeaponData(Entity* entity) noexcept : BaseData{ entity }
{
    clip = entity->clip();
    reserveAmmo = entity->reserveAmmoCount();

    if (const auto weaponInfo = entity->getWeaponData()) {
        group = [](WeaponType type, WeaponId weaponId) {
            switch (type) {
            case WeaponType::Pistol: return "Pistols";
            case WeaponType::SubMachinegun: return "SMGs";
            case WeaponType::Rifle: return "Rifles";
            case WeaponType::SniperRifle: return "Sniper Rifles";
            case WeaponType::Shotgun: return "Shotguns";
            case WeaponType::Machinegun: return "Machineguns";
            case WeaponType::Grenade: return "Grenades";
            case WeaponType::Melee: return "Melee";
            default:
                switch (weaponId) {
                case WeaponId::C4:
                case WeaponId::Healthshot:
                case WeaponId::BumpMine:
                case WeaponId::ZoneRepulsor:
                case WeaponId::Shield:
                    return "Other";
                default: return "All";
                }
            }
        }(weaponInfo->type, entity->itemDefinitionIndex2());
        name = [](WeaponId weaponId) {
            switch (weaponId) {
            default: return "All";

            case WeaponId::Glock: return "Glock-18";
            case WeaponId::Hkp2000: return "P2000";
            case WeaponId::Usp_s: return "USP-S";
            case WeaponId::Elite: return "Dual Berettas";
            case WeaponId::P250: return "P250";
            case WeaponId::Tec9: return "Tec-9";
            case WeaponId::Fiveseven: return "Five-SeveN";
            case WeaponId::Cz75a: return "CZ75-Auto";
            case WeaponId::Deagle: return "Desert Eagle";
            case WeaponId::Revolver: return "R8 Revolver";

            case WeaponId::Mac10: return "MAC-10";
            case WeaponId::Mp9: return "MP9";
            case WeaponId::Mp7: return "MP7";
            case WeaponId::Mp5sd: return "MP5-SD";
            case WeaponId::Ump45: return "UMP-45";
            case WeaponId::P90: return "P90";
            case WeaponId::Bizon: return "PP-Bizon";

            case WeaponId::GalilAr: return "Galil AR";
            case WeaponId::Famas: return "FAMAS";
            case WeaponId::Ak47: return "AK-47";
            case WeaponId::M4A1: return "M4A4";
            case WeaponId::M4a1_s: return "M4A1-S";
            case WeaponId::Sg553: return "SG 553";
            case WeaponId::Aug: return "AUG";

            case WeaponId::Ssg08: return "SSG 08";
            case WeaponId::Awp: return "AWP";
            case WeaponId::G3SG1: return "G3SG1";
            case WeaponId::Scar20: return "SCAR-20";

            case WeaponId::Nova: return "Nova";
            case WeaponId::Xm1014: return "XM1014";
            case WeaponId::Sawedoff: return "Sawed-Off";
            case WeaponId::Mag7: return "MAG-7";

            case WeaponId::M249: return "M249";
            case WeaponId::Negev: return "Negev";

            case WeaponId::Flashbang: return "Flashbang";
            case WeaponId::HeGrenade: return "HE Grenade";
            case WeaponId::SmokeGrenade: return "Smoke Grenade";
            case WeaponId::Molotov: return "Molotov";
            case WeaponId::Decoy: return "Decoy Grenade";
            case WeaponId::IncGrenade: return "Incendiary";
            case WeaponId::TaGrenade: return "TA Grenade";
            case WeaponId::Firebomb: return "Fire Bomb";
            case WeaponId::Diversion: return "Diversion";
            case WeaponId::FragGrenade: return "Frag Grenade";
            case WeaponId::Snowball: return "Snowball";

            case WeaponId::Axe: return "Axe";
            case WeaponId::Hammer: return "Hammer";
            case WeaponId::Spanner: return "Wrench";

            case WeaponId::C4: return "C4";
            case WeaponId::Healthshot: return "Healthshot";
            case WeaponId::BumpMine: return "Bump Mine";
            case WeaponId::ZoneRepulsor: return "Zone Repulsor";
            case WeaponId::Shield: return "Shield";
            }
        }(entity->itemDefinitionIndex2());

        displayName = interfaces->localize->findAsUTF8(weaponInfo->name);
    }
}

LootCrateData::LootCrateData(Entity* entity) noexcept : BaseData{ entity }
{
    const auto model = entity->getModel();
    if (!model)
        return;

    name = [](const char* modelName) -> const char* {
        switch (fnv::hashRuntime(modelName)) {
        case fnv::hash("models/props_survival/cases/case_pistol.mdl"): return "Pistol Case";
        case fnv::hash("models/props_survival/cases/case_light_weapon.mdl"): return "Light Case";
        case fnv::hash("models/props_survival/cases/case_heavy_weapon.mdl"): return "Heavy Case";
        case fnv::hash("models/props_survival/cases/case_explosive.mdl"): return "Explosive Case";
        case fnv::hash("models/props_survival/cases/case_tools.mdl"): return "Tools Case";
        case fnv::hash("models/props_survival/cash/dufflebag.mdl"): return "Cash Dufflebag";
        default: return nullptr;
        }
    }(model->name);
}

ObserverData::ObserverData(Entity* entity, Entity* obs, bool targetIsLocalPlayer) noexcept : playerHandle{ entity->handle() }, targetHandle{ obs->handle() }, targetIsLocalPlayer{ targetIsLocalPlayer } {}

void BombData::update() noexcept
{
    if (memory->plantedC4s->size > 0 && (!*memory->gameRules || (*memory->gameRules)->mapHasBombTarget())) {
        if (const auto bomb = (*memory->plantedC4s)[0]; bomb && bomb->c4Ticking()) {
            blowTime = bomb->c4BlowTime();
            timerLength = bomb->c4TimerLength();
            defuserHandle = bomb->c4Defuser();
            if (defuserHandle != -1) {
                defuseCountDown = bomb->c4DefuseCountDown();
                defuseLength = bomb->c4DefuseLength();
            }

            if (*memory->playerResource) {
                const auto& bombOrigin = bomb->origin();
                bombsite = bombOrigin.distTo((*memory->playerResource)->bombsiteCenterA()) > bombOrigin.distTo((*memory->playerResource)->bombsiteCenterB());
            }
            return;
        }
    }
    blowTime = 0.0f;
}

InfernoData::InfernoData(Entity* inferno) noexcept
{
    const auto& origin = inferno->getAbsOrigin();

    owner = interfaces->entityList->getEntityFromHandle(inferno->ownerEntity());

    points.reserve(inferno->fireCount());
    for (int i = 0; i < inferno->fireCount(); ++i) {
        if (inferno->fireIsBurning()[i])
            points.emplace_back(inferno->fireXDelta()[i] + origin.x, inferno->fireYDelta()[i] + origin.y, inferno->fireZDelta()[i] + origin.z);
    }
}

SmokeData::SmokeData(Entity* smoke) noexcept
{
    origin = smoke->getAbsOrigin();
}
