// SPDX-License-Identifier: GPL-3.0-or-later
//
// KEDAVRA frame generation inside Gamescope. See kfg.hpp for the data flow.

#include "kfg.hpp"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "convar.h"
#include "log.hpp"
#include "rendervulkan.hpp"

#include "kfg_plan.h"
#include "kfg_sched.h"

uint64_t get_time_in_nanos();

namespace kfg
{
	static LogScope kfg_log( "kfg" );

	gamescope::ConVar<int> cv_kfg_multiplier{ "kfg_multiplier", 0, "KEDAVRA frame generation: 0 = off, 2 = x2, 3 = x3." };
	gamescope::ConVar<std::string> cv_kfg_trace{ "kfg_trace", "", "CSV trace of every KMS flip while frame generation is on (kedavra/tools/kfg_pacing.py). Empty = off." };
	gamescope::ConVar<std::string> cv_kfg_status{ "kfg_status", "", "JSON status file, rewritten once per second. Empty = $XDG_RUNTIME_DIR/kedavra-status.json." };
	gamescope::ConVar<bool> cv_kfg_debug_marker{ "kfg_debug_marker", false, "Mark generated frames with a corner square, for camera checks." };
	gamescope::ConVar<int> cv_kfg_budget_us{ "kfg_budget_us", 4000, "GPU time of KEDAVRA passes per composite above which a frame counts as over budget." };

	// Frames kept on the GPU. The scheduler may show content up to max_delay vblanks
	// behind the newest latch, which at x3 is up to five source frames.
	static constexpr uint32_t k_uRing = 6;
	static constexpr uint32_t k_uTimingSlots = 16;

	struct Frame_t
	{
		uint64_t ulID = 0;
		uint64_t ulPrevID = 0;      // the frame the pair analysis of this one used; 0 = none
		bool bValid = false;
		gamescope::OwningRc<CVulkanTexture> pColor;
		gamescope::OwningRc<CVulkanTexture> pLuma[ KFG_LEVELS + 1 ];
		gamescope::OwningRc<CVulkanTexture> pFlow[ KFG_LEVELS + 1 ];  // pair (ulPrevID, ulID)
		gamescope::OwningRc<CVulkanTexture> pStats;
	};

	struct TraceRow_t
	{
		int64_t nVBlank;
		uint64_t ulFlipNs;
		kfg_decision decision;
		bool bBypass;          // the composite did not apply the decision
		uint64_t ulRefreshNs;
		uint32_t uMultiplier;
	};

	struct State_t
	{
		kfg_sched sched;
		bool bInit = false;
		uint32_t uMultiplier = 1;

		// Vblank grid: index = round((target - origin) / period) + base.
		uint64_t ulGridOrigin = 0;
		uint64_t ulGridPeriod = 0;
		int64_t nGridBase = 0;
		int64_t nLastVBlank = -1;

		uint64_t ulLastCommitID = 0;
		uint32_t uLastAppID = 0;
		uint32_t uWidth = 0, uHeight = 0;

		Frame_t frames[ k_uRing ];
		uint32_t uNextFrame = 0;
		gamescope::OwningRc<CVulkanTexture> pSelect;
		gamescope::OwningRc<CVulkanTexture> pOutput[ 2 ];
		uint32_t uNextOutput = 0;
		gamescope::Rc<CVulkanTexture> pLastShown;  // what REPEAT shows again

		// Paint in progress.
		bool bPending = false;
		bool bApplied = false;
		bool bNewSource = false;
		uint64_t ulPendingID = 0;
		int64_t nPendingVBlank = 0;
		kfg_decision pending{};

		// Counters (status file).
		uint64_t ulPaints = 0, ulApplied = 0, ulBypass = 0, ulRingMiss = 0, ulUnsupported = 0;
		uint64_t ulOverBudget = 0, ulSources = 0;
		const char *pszLastProblem = "none";
		double flGpuUsLast = 0, flGpuUsMax = 0, flGpuUsSum = 0;
		uint64_t ulGpuSamples = 0;
		uint64_t ulLastStatusNs = 0;

		// GPU timestamps.
		VkQueryPool queryPool = VK_NULL_HANDLE;
		bool bQueryUnsupported = false;
		double flPeriodNs = 1.0;
		uint64_t ulValidMask = ~0ull;
		uint32_t uNextQuery = 0;
		std::deque<uint32_t> pendingQueries;

		// Trace.
		FILE *pTrace = nullptr;
		std::string sTracePath;
	};

	static State_t g_State;

	// Presented decisions waiting for their page flip (steamcompmgr -> KMS thread)
	// and flipped rows waiting to be written (KMS thread -> steamcompmgr).
	static std::mutex g_TraceMutex;
	static std::deque<TraceRow_t> g_InFlight;
	static std::vector<TraceRow_t> g_Flipped;

	static ShaderType ShaderFor( kfg_shader eShader )
	{
		switch ( eShader )
		{
			case KFG_SHADER_REDUCE: return SHADER_TYPE_KFG_REDUCE;
			case KFG_SHADER_ME:     return SHADER_TYPE_KFG_ME;
			case KFG_SHADER_STATS:  return SHADER_TYPE_KFG_STATS;
			case KFG_SHADER_SELECT: return SHADER_TYPE_KFG_SELECT;
			case KFG_SHADER_WARP:
			default:                return SHADER_TYPE_KFG_WARP;
		}
	}

	static uint32_t DrmFormatFor( kfg_format eFormat )
	{
		switch ( eFormat )
		{
			case KFG_FORMAT_R8:      return DRM_FORMAT_R8;
			case KFG_FORMAT_RGBA16F: return DRM_FORMAT_ABGR16161616F;
			case KFG_FORMAT_RGBA8:
			default:                 return DRM_FORMAT_ABGR8888;
		}
	}

	static gamescope::OwningRc<CVulkanTexture> CreateImage( uint32_t uWidth, uint32_t uHeight, kfg_format eFormat )
	{
		CVulkanTexture::createFlags flags;
		flags.bSampled = true;
		flags.bStorage = true;
		gamescope::OwningRc<CVulkanTexture> pTex = new CVulkanTexture();
		if ( !pTex->BInit( uWidth, uHeight, 1u, DrmFormatFor( eFormat ), flags ) )
			return nullptr;
		return pTex;
	}

	static bool SupportedSource( const FrameInfo_t::Layer_t &layer )
	{
		if ( !layer.tex || layer.isYcbcr() )
			return false;
		switch ( layer.tex->format() )
		{
			case VK_FORMAT_B8G8R8A8_UNORM:
			case VK_FORMAT_B8G8R8A8_SRGB:
			case VK_FORMAT_R8G8B8A8_UNORM:
			case VK_FORMAT_R8G8B8A8_SRGB:
				return true;
			default:
				return false;
		}
	}

	static void ReleaseImages( State_t &s )
	{
		for ( Frame_t &f : s.frames )
			f = Frame_t{};
		s.pSelect = nullptr;
		s.pOutput[ 0 ] = s.pOutput[ 1 ] = nullptr;
		s.pLastShown = nullptr;
		s.uWidth = s.uHeight = 0;
	}

	static bool EnsureImages( State_t &s, uint32_t uWidth, uint32_t uHeight )
	{
		if ( s.uWidth == uWidth && s.uHeight == uHeight && s.pOutput[ 0 ] )
			return true;
		ReleaseImages( s );
		for ( Frame_t &f : s.frames )
		{
			f.pColor = CreateImage( uWidth, uHeight, KFG_FORMAT_RGBA8 );
			if ( !f.pColor )
				return false;
			for ( uint32_t l = 1; l <= KFG_LEVELS; l++ )
			{
				uint32_t sz[ 2 ];
				kfg_level_size( uWidth, uHeight, l, sz );
				f.pLuma[ l ] = CreateImage( sz[ 0 ], sz[ 1 ], KFG_FORMAT_R8 );
				if ( l >= 2 )
					f.pFlow[ l ] = CreateImage( sz[ 0 ], sz[ 1 ], KFG_FORMAT_RGBA16F );
				if ( !f.pLuma[ l ] || ( l >= 2 && !f.pFlow[ l ] ) )
					return false;
			}
			f.pStats = CreateImage( 1, 1, KFG_FORMAT_RGBA16F );
			if ( !f.pStats )
				return false;
		}
		uint32_t bs[ 2 ];
		kfg_select_size( uWidth, uHeight, bs );
		s.pSelect = CreateImage( bs[ 0 ], bs[ 1 ], KFG_FORMAT_RGBA16F );
		s.pOutput[ 0 ] = CreateImage( uWidth, uHeight, KFG_FORMAT_RGBA8 );
		s.pOutput[ 1 ] = CreateImage( uWidth, uHeight, KFG_FORMAT_RGBA8 );
		if ( !s.pSelect || !s.pOutput[ 0 ] || !s.pOutput[ 1 ] )
			return false;
		s.uWidth = uWidth;
		s.uHeight = uHeight;
		kfg_log.infof( "ring of %u frames at %ux%u allocated", k_uRing, uWidth, uHeight );
		return true;
	}

	static Frame_t *FindFrame( State_t &s, uint64_t ulID )
	{
		for ( Frame_t &f : s.frames )
		{
			if ( f.bValid && f.ulID == ulID )
				return &f;
		}
		return nullptr;
	}

	static uint32_t FrameIndex( State_t &s, const Frame_t *pFrame )
	{
		return uint32_t( pFrame - s.frames );
	}

	static void ResetHistory( State_t &s, const char *pszWhy )
	{
		for ( Frame_t &f : s.frames )
		{
			f.bValid = false;
			f.ulPrevID = 0;
		}
		s.pLastShown = nullptr;
		if ( s.bInit )
			kfg_sched_reset_history( &s.sched );
		s.ulLastCommitID = 0;
		kfg_log.debugf( "history reset: %s", pszWhy );
	}

	// ---------------------------------------------------------------- GPU timing

	static bool TimingInit( State_t &s )
	{
		if ( s.queryPool != VK_NULL_HANDLE || s.bQueryUnsupported )
			return s.queryPool != VK_NULL_HANDLE;

		uint32_t uFamilyCount = 0;
		g_device.vk.GetPhysicalDeviceQueueFamilyProperties( g_device.physDev(), &uFamilyCount, nullptr );
		std::vector<VkQueueFamilyProperties> families( uFamilyCount );
		g_device.vk.GetPhysicalDeviceQueueFamilyProperties( g_device.physDev(), &uFamilyCount, families.data() );
		if ( g_device.queueFamily() >= uFamilyCount || families[ g_device.queueFamily() ].timestampValidBits == 0 )
		{
			s.bQueryUnsupported = true;
			return false;
		}
		const uint32_t uBits = families[ g_device.queueFamily() ].timestampValidBits;
		s.ulValidMask = uBits >= 64 ? ~0ull : ( ( 1ull << uBits ) - 1 );

		VkPhysicalDeviceProperties props;
		g_device.vk.GetPhysicalDeviceProperties( g_device.physDev(), &props );
		s.flPeriodNs = props.limits.timestampPeriod;

		VkQueryPoolCreateInfo createInfo =
		{
			.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
			.queryType = VK_QUERY_TYPE_TIMESTAMP,
			.queryCount = k_uTimingSlots * 2,
		};
		if ( g_device.vk.CreateQueryPool( g_device.device(), &createInfo, nullptr, &s.queryPool ) != VK_SUCCESS )
		{
			s.bQueryUnsupported = true;
			return false;
		}
		return true;
	}

	static void TimingDrain( State_t &s )
	{
		while ( !s.pendingQueries.empty() )
		{
			const uint32_t uSlot = s.pendingQueries.front();
			uint64_t ulResults[ 4 ] = {};
			VkResult res = g_device.vk.GetQueryPoolResults( g_device.device(), s.queryPool, uSlot * 2, 2, sizeof( ulResults ), ulResults,
				2 * sizeof( uint64_t ), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT );
			if ( res == VK_NOT_READY || !ulResults[ 1 ] || !ulResults[ 3 ] )
				return;
			s.pendingQueries.pop_front();
			const uint64_t ulTicks = ( ulResults[ 2 ] - ulResults[ 0 ] ) & s.ulValidMask;
			const double flUs = double( ulTicks ) * s.flPeriodNs / 1000.0;
			s.flGpuUsLast = flUs;
			s.flGpuUsMax = std::max( s.flGpuUsMax, flUs );
			s.flGpuUsSum += flUs;
			s.ulGpuSamples++;
			if ( flUs > double( cv_kfg_budget_us.Get() ) )
				s.ulOverBudget++;
		}
	}

	// ---------------------------------------------------------------- recording

	struct Resolve_t
	{
		State_t *s;
		gamescope::Rc<CVulkanTexture> pSourceColor;  // KFG_ROLE_COLOR of the frame being copied
		uint32_t uOutput;
	};

	static CVulkanTexture *ResolveRef( Resolve_t &r, kfg_image_ref ref )
	{
		State_t &s = *r.s;
		switch ( ref.role )
		{
			case KFG_ROLE_COLOR:  return s.frames[ ref.ring ].pColor.get();
			case KFG_ROLE_LUMA:   return s.frames[ ref.ring ].pLuma[ ref.level ].get();
			case KFG_ROLE_FLOW:   return s.frames[ ref.ring ].pFlow[ ref.level ].get();
			case KFG_ROLE_STATS:  return s.frames[ ref.ring ].pStats.get();
			case KFG_ROLE_SELECT: return s.pSelect.get();
			case KFG_ROLE_OUTPUT: return s.pOutput[ r.uOutput ].get();
			default:              return nullptr;
		}
	}

	static void RecordPlan( CVulkanCmdBuffer *pCmd, const kfg_plan &plan, Resolve_t &r )
	{
		for ( uint32_t i = 0; i < plan.n; i++ )
		{
			const kfg_pass &pass = plan.pass[ i ];
			pCmd->bindPipeline( g_device.pipeline( ShaderFor( pass.shader ) ) );
			for ( uint32_t slot = 0; slot < KFG_MAX_SLOTS; slot++ )
			{
				CVulkanTexture *pTex = slot < pass.nslots ? ResolveRef( r, pass.slots[ slot ] ) : nullptr;
				pCmd->bindTexture( slot, pTex );
				pCmd->setTextureSrgb( slot, false );
				pCmd->setSamplerNearest( slot, false );
				pCmd->setSamplerUnnormalized( slot, false );
			}
			pCmd->bindTarget( ResolveRef( r, pass.dst ) );
			UploadParams( pCmd, pass.params );
			pCmd->dispatch( pass.groups[ 0 ], pass.groups[ 1 ] );
		}
	}

	static void RecordCopy( CVulkanCmdBuffer *pCmd, gamescope::Rc<CVulkanTexture> pSrc, CVulkanTexture *pDst )
	{
		kfg_params p = {};
		p.dst_size[ 0 ] = int32_t( pDst->width() );
		p.dst_size[ 1 ] = int32_t( pDst->height() );
		p.src_size[ 0 ] = p.dst_size[ 0 ];
		p.src_size[ 1 ] = p.dst_size[ 1 ];
		pCmd->bindPipeline( g_device.pipeline( SHADER_TYPE_KFG_COPY ) );
		pCmd->bindTexture( 0, std::move( pSrc ) );
		pCmd->setTextureSrgb( 0, false );
		pCmd->setSamplerNearest( 0, true );
		pCmd->setSamplerUnnormalized( 0, false );
		for ( uint32_t slot = 1; slot < KFG_MAX_SLOTS; slot++ )
			pCmd->bindTexture( slot, nullptr );
		pCmd->bindTarget( pDst );
		UploadParams( pCmd, p );
		pCmd->dispatch( kfg_div_up( pDst->width(), 8 ), kfg_div_up( pDst->height(), 8 ) );
	}

	// Writes from earlier composites (ring frames, analysis) become visible to this one.
	// Gamescope's per-command-buffer tracking only emits barriers for images written
	// in the same command buffer.
	static void RecordHistoryBarrier( CVulkanCmdBuffer *pCmd )
	{
		VkMemoryBarrier barrier =
		{
			.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
			.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
		};
		g_device.vk.CmdPipelineBarrier( pCmd->rawBuffer(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			0, 1, &barrier, 0, nullptr, 0, nullptr );
	}

	// ---------------------------------------------------------------- trace and status

	static void WriteTrace( State_t &s )
	{
		std::vector<TraceRow_t> rows;
		{
			std::scoped_lock lock( g_TraceMutex );
			rows.swap( g_Flipped );
		}

		const std::string &sPath = cv_kfg_trace.Get();
		if ( sPath != s.sTracePath )
		{
			if ( s.pTrace )
				fclose( s.pTrace );
			s.pTrace = nullptr;
			s.sTracePath = sPath;
			if ( !sPath.empty() )
			{
				s.pTrace = fopen( sPath.c_str(), "w" );
				if ( s.pTrace )
				{
					fprintf( s.pTrace, "vblank,flip_ns,kind,src_a,src_b,phase_num,phase_den,content,delay,reason,state,refresh_ns,mult,compose_late,skipped\n" );
					kfg_log.infof( "trace: %s", sPath.c_str() );
				}
				else
					kfg_log.errorf( "trace: cannot open %s", sPath.c_str() );
			}
		}
		if ( !s.pTrace )
			return;

		for ( const TraceRow_t &row : rows )
		{
			const kfg_decision &d = row.decision;
			const char *pszKind = row.bBypass ? "repeat" : kfg_show_name( d.show );
			const char *pszReason = row.bBypass ? "bypass" : kfg_reason_name( d.reason );
			fprintf( s.pTrace, "%" PRId64 ",%" PRIu64 ",%s,%" PRIu64 ",%" PRIu64 ",%u,%u,%" PRId64 ",%" PRId64 ",%s,%s,%" PRIu64 ",%u,0,%d\n",
				row.nVBlank, row.ulFlipNs, pszKind, d.src_a, d.src_b, d.phase_num, d.phase_den, d.content, d.delay,
				pszReason, kfg_state_name( d.state ), row.ulRefreshNs, row.uMultiplier, d.skipped ? 1 : 0 );
		}
		if ( !rows.empty() )
			fflush( s.pTrace );
	}

	static void WriteStatus( State_t &s, uint64_t ulNow )
	{
		if ( ulNow - s.ulLastStatusNs < 1'000'000'000ull )
			return;
		s.ulLastStatusNs = ulNow;

		std::string sPath = cv_kfg_status.Get();
		if ( sPath.empty() )
		{
			const char *pszRuntime = getenv( "XDG_RUNTIME_DIR" );
			if ( !pszRuntime )
				return;
			sPath = std::string( pszRuntime ) + "/kedavra-status.json";
		}
		const std::string sTmp = sPath + ".tmp";
		FILE *f = fopen( sTmp.c_str(), "w" );
		if ( !f )
			return;
		const kfg_sched &q = s.sched;
		fprintf( f,
			"{\"engine\":\"kedavra-gamescope\",\"version\":1,\"time_ns\":%" PRIu64 ",\"multiplier\":%u,\"state\":\"%s\","
			"\"refresh_ns\":%" PRIu64 ",\"width\":%u,\"height\":%u,"
			"\"paints\":%" PRIu64 ",\"applied\":%" PRIu64 ",\"bypass\":%" PRIu64 ",\"ring_miss\":%" PRIu64 ",\"unsupported\":%" PRIu64 ","
			"\"sources\":%" PRIu64 ",\"real\":%" PRIu64 ",\"gen\":%" PRIu64 ",\"repeat\":%" PRIu64 ",\"skip\":%" PRIu64 ",\"resync\":%" PRIu64 ",\"degrade\":%" PRIu64 ","
			"\"gpu_us_last\":%.1f,\"gpu_us_max\":%.1f,\"gpu_us_avg\":%.1f,\"over_budget\":%" PRIu64 ",\"timestamps\":%s,"
			"\"last_problem\":\"%s\",\"trace\":\"%s\"}\n",
			ulNow, s.uMultiplier, s.bInit ? kfg_state_name( q.state ) : "off",
			s.ulGridPeriod, s.uWidth, s.uHeight,
			s.ulPaints, s.ulApplied, s.ulBypass, s.ulRingMiss, s.ulUnsupported,
			s.ulSources, q.n_real, q.n_gen, q.n_repeat, q.n_skip, q.n_resync, q.n_degrade,
			s.flGpuUsLast, s.flGpuUsMax, s.ulGpuSamples ? s.flGpuUsSum / double( s.ulGpuSamples ) : 0.0, s.ulOverBudget,
			s.queryPool != VK_NULL_HANDLE ? "true" : "false",
			s.pszLastProblem, s.pTrace ? s.sTracePath.c_str() : "" );
		fclose( f );
		rename( sTmp.c_str(), sPath.c_str() );
	}

	// ---------------------------------------------------------------- entry points

	static uint32_t RequestedMultiplier()
	{
		const int n = cv_kfg_multiplier.Get();
		return ( n == 2 || n == 3 ) ? uint32_t( n ) : 1u;
	}

	bool WantsEveryVBlank()
	{
		return RequestedMultiplier() > 1 || ( g_State.bInit && g_State.sched.state != KFG_STATE_OFF );
	}

	void OnPaint( FrameInfo_t *pFrameInfo, uint64_t ulBaseCommitID, uint32_t uAppID, uint64_t ulTargetVBlankNs, uint64_t ulRefreshCycleNs )
	{
		State_t &s = g_State;
		const uint64_t ulNow = get_time_in_nanos();
		s.bPending = false;
		s.bApplied = false;

		WriteTrace( s );
		WriteStatus( s, ulNow );

		const uint32_t uMult = RequestedMultiplier();
		if ( !s.bInit )
		{
			if ( uMult == 1 )
				return;
			kfg_sched_config cfg = kfg_sched_default_config( uMult );
			kfg_sched_init( &s.sched, &cfg );
			s.uMultiplier = uMult;
			s.bInit = true;
		}
		if ( uMult != s.uMultiplier )
		{
			kfg_sched_set_multiplier( &s.sched, uMult );
			s.uMultiplier = uMult;
			kfg_log.infof( "multiplier %u", uMult );
		}
		if ( uMult == 1 && s.sched.state == KFG_STATE_OFF )
		{
			// Fully off: drop the ring so an idle feature costs no memory.
			if ( s.uWidth )
				ReleaseImages( s );
			return;
		}

		if ( !pFrameInfo->layers.count() || !SupportedSource( pFrameInfo->layers.get( 0 ) ) )
		{
			s.ulUnsupported++;
			s.pszLastProblem = "unsupported_layer0";
			ResetHistory( s, "unsupported layer 0" );
			return;
		}

		// Vblank index of this paint.
		const uint64_t ulPeriod = ulRefreshCycleNs ? ulRefreshCycleNs : 11'111'111ull;
		if ( !s.ulGridPeriod || ulPeriod != s.ulGridPeriod )
		{
			s.ulGridOrigin = ulTargetVBlankNs;
			s.ulGridPeriod = ulPeriod;
			s.nGridBase = s.nLastVBlank + 1;
			ResetHistory( s, "refresh change" );
		}
		const double flSteps = ( double( int64_t( ulTargetVBlankNs - s.ulGridOrigin ) ) ) / double( s.ulGridPeriod );
		int64_t nVBlank = s.nGridBase + int64_t( std::llround( flSteps ) );
		if ( nVBlank <= s.nLastVBlank )
			nVBlank = s.nLastVBlank + 1;  // two paints for one vblank: keep indices strictly increasing
		s.nLastVBlank = nVBlank;

		if ( uAppID != s.uLastAppID )
		{
			s.uLastAppID = uAppID;
			ResetHistory( s, "focus change" );
		}

		const FrameInfo_t::Layer_t &layer0 = pFrameInfo->layers.get( 0 );
		if ( layer0.tex->width() != s.uWidth || layer0.tex->height() != s.uHeight )
		{
			ResetHistory( s, "size change" );
			if ( !EnsureImages( s, layer0.tex->width(), layer0.tex->height() ) )
			{
				s.pszLastProblem = "allocation_failed";
				ReleaseImages( s );
				return;
			}
		}

		s.bNewSource = ulBaseCommitID != 0 && ulBaseCommitID != s.ulLastCommitID;
		if ( s.bNewSource )
		{
			s.ulLastCommitID = ulBaseCommitID;
			s.ulSources++;
			kfg_sched_on_latch( &s.sched, ulBaseCommitID, nVBlank );
			// Analysis is recorded ahead of generation in this paint's composite.
			kfg_sched_on_me( &s.sched, ulBaseCommitID, nVBlank, true, false );
		}

		s.pending = kfg_sched_decide( &s.sched, nVBlank );
		s.ulPendingID = ulBaseCommitID;
		s.nPendingVBlank = nVBlank;
		s.bPending = true;
		s.ulPaints++;

		// The composite must run: generated content never matches a held commit.
		pFrameInfo->bRepeatFrame = false;
	}

	void RecordComposite( CVulkanCmdBuffer *pCmd, FrameInfo_t *pFrameInfo )
	{
		State_t &s = g_State;
		if ( !s.bPending || s.bApplied || !pFrameInfo->layers.count() )
			return;
		s.bApplied = true;

		FrameInfo_t::Layer_t &layer0 = pFrameInfo->layers.get( 0 );
		if ( !SupportedSource( layer0 ) || layer0.tex->width() != s.uWidth || layer0.tex->height() != s.uHeight )
			return;

		const bool bTiming = TimingInit( s );
		if ( bTiming )
			TimingDrain( s );
		std::optional<uint32_t> oQuery;
		if ( bTiming && s.pendingQueries.size() < k_uTimingSlots )
		{
			oQuery = s.uNextQuery;
			s.uNextQuery = ( s.uNextQuery + 1 ) % k_uTimingSlots;
			g_device.vk.CmdResetQueryPool( pCmd->rawBuffer(), s.queryPool, *oQuery * 2, 2 );
			g_device.vk.CmdWriteTimestamp( pCmd->rawBuffer(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, s.queryPool, *oQuery * 2 );
		}

		RecordHistoryBarrier( pCmd );

		Resolve_t r = { &s, layer0.tex, s.uNextOutput };
		const kfg_tuning tune = kfg_default_tuning();
		kfg_plan plan;

		if ( s.bNewSource )
		{
			Frame_t *pPrev = nullptr;
			// The newest valid frame is the previous source (ids increase).
			for ( Frame_t &f : s.frames )
			{
				if ( f.bValid && f.ulID < s.ulPendingID && ( !pPrev || f.ulID > pPrev->ulID ) )
					pPrev = &f;
			}
			Frame_t &cur = s.frames[ s.uNextFrame ];
			s.uNextFrame = ( s.uNextFrame + 1 ) % k_uRing;
			if ( pPrev == &cur )
				pPrev = nullptr;

			RecordCopy( pCmd, layer0.tex, cur.pColor.get() );
			cur.ulID = s.ulPendingID;
			cur.bValid = true;
			cur.ulPrevID = pPrev ? pPrev->ulID : 0;
			const uint32_t fb = FrameIndex( s, &cur );
			const uint32_t fa = pPrev ? FrameIndex( s, pPrev ) : fb;
			kfg_plan_analyze( &plan, s.uWidth, s.uHeight, fa, fb, fb, pPrev ? 1 : 0, &tune );
			RecordPlan( pCmd, plan, r );
		}

		const kfg_decision &d = s.pending;
		gamescope::Rc<CVulkanTexture> pShow;
		switch ( d.show )
		{
			case KFG_SHOW_REAL:
			{
				Frame_t *pA = FindFrame( s, d.src_a );
				if ( pA )
					pShow = pA->pColor.get();
				else
					s.ulRingMiss++, s.pszLastProblem = "ring_miss_real";
				break;
			}
			case KFG_SHOW_GEN:
			{
				Frame_t *pA = FindFrame( s, d.src_a );
				Frame_t *pB = FindFrame( s, d.src_b );
				if ( pA && pB && pB->ulPrevID == pA->ulID )
				{
					kfg_plan_generate( &plan, s.uWidth, s.uHeight, FrameIndex( s, pA ), FrameIndex( s, pB ), FrameIndex( s, pB ),
						float( d.phase_num ) / float( d.phase_den ), cv_kfg_debug_marker ? KFG_FLAG_DEBUG_MARKER : 0 );
					RecordPlan( pCmd, plan, r );
					pShow = s.pOutput[ s.uNextOutput ].get();
					s.uNextOutput ^= 1u;
				}
				else
				{
					s.ulRingMiss++;
					s.pszLastProblem = "ring_miss_pair";
					if ( pB )
						pShow = pB->pColor.get();
				}
				break;
			}
			case KFG_SHOW_REPEAT:
				pShow = s.pLastShown;
				break;
			case KFG_SHOW_NONE:
			default:
				break;
		}

		if ( oQuery )
		{
			g_device.vk.CmdWriteTimestamp( pCmd->rawBuffer(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s.queryPool, *oQuery * 2 + 1 );
			s.pendingQueries.push_back( *oQuery );
		}

		if ( d.state == KFG_STATE_OFF || !pShow )
		{
			// Passthrough: the newest commit as Gamescope would show it.
			s.pLastShown = nullptr;
			return;
		}
		layer0.tex = pShow;
		s.pLastShown = pShow;
		s.ulApplied++;
	}

	void OnPresented( bool bPresented )
	{
		State_t &s = g_State;
		if ( !s.bPending )
			return;
		s.bPending = false;
		if ( !bPresented )
			return;
		const bool bBypass = !s.bApplied && s.pending.state != KFG_STATE_OFF;
		if ( bBypass )
		{
			s.ulBypass++;
			s.pszLastProblem = "composite_bypassed";
		}
		if ( cv_kfg_trace.Get().empty() )
			return;
		std::scoped_lock lock( g_TraceMutex );
		if ( g_InFlight.size() > 64 )
			g_InFlight.clear();  // flips stopped arriving (suspend, DPMS)
		g_InFlight.push_back( TraceRow_t{ s.nPendingVBlank, 0, s.pending, bBypass, s.ulGridPeriod, s.uMultiplier } );
	}

	void OnPageFlip( uint64_t ulVBlankNs, uint32_t uSequence )
	{
		(void) uSequence;
		std::scoped_lock lock( g_TraceMutex );
		if ( g_InFlight.empty() )
			return;
		TraceRow_t row = g_InFlight.front();
		g_InFlight.pop_front();
		row.ulFlipNs = ulVBlankNs;
		if ( g_Flipped.size() < 4096 )
			g_Flipped.push_back( row );
	}
}
