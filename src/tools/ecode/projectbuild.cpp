#include "projectbuild.hpp"
#include "scopedop.hpp"
#include <eepp/core/string.hpp>
#include <eepp/system/filesystem.hpp>
#include <eepp/system/log.hpp>
#include <nlohmann/json.hpp>
using json = nlohmann::json;

using namespace EE;

namespace ecode {

static const char* PROJECT_ROOT = "${project_root}";

void ProjectBuild::replaceVars() {
	const std::vector<ProjectBuildSteps*> steps{ &mBuild, &mClean };
	auto replaceVar = []( ProjectBuildStep& s, const std::string& var, const std::string& val ) {
		String::replaceAll( s.workingDir, var, val );
		String::replaceAll( s.cmd, var, val );
		String::replaceAll( s.args, var, val );
	};

	for ( auto& step : steps ) {
		for ( auto& s : *step ) {
			replaceVar( s, PROJECT_ROOT, mProjectRoot );
			for ( auto& var : mVars ) {
				std::string varKey( "${" + var.first + "}" );
				String::replaceAll( var.second, PROJECT_ROOT, mProjectRoot );
				replaceVar( s, varKey, var.second );
			}
		}
	}
}

ProjectBuildManager::ProjectBuildManager( const std::string& projectRoot,
										  std::shared_ptr<ThreadPool> pool ) :
	mProjectRoot( projectRoot ), mThreadPool( pool ) {
	FileSystem::dirAddSlashAtEnd( mProjectRoot );

	if ( mThreadPool ) {
		mThreadPool->run( [this]() { load(); } );
	} else {
		load();
	}
};

static bool isValidType( const std::string& typeStr ) {
	return "error" == typeStr || "warning" == typeStr || "notice" == typeStr;
}

static ProjectOutputParserTypes outputParserType( const std::string& typeStr ) {
	if ( "error" == typeStr )
		return ProjectOutputParserTypes::Error;
	if ( "warning" == typeStr )
		return ProjectOutputParserTypes::Warning;
	if ( "notice" == typeStr )
		return ProjectOutputParserTypes::Notice;
	return ProjectOutputParserTypes::Notice;
}

bool ProjectBuildManager::load() {
	ScopedOp op( [this]() { mLoading = true; }, [this]() { mLoading = false; } );

	mProjectFile = mProjectRoot + ".ecode/project-build.json";
	if ( !FileSystem::fileExists( mProjectFile ) )
		return false;
	std::string data;
	if ( !FileSystem::fileGet( mProjectFile, data ) )
		return false;
	json j;

	try {
		j = json::parse( data, nullptr, true, true );
	} catch ( const json::exception& e ) {
		Log::error( "ProjectBuildManager::load - Error parsing project build config from "
					"path %s, error: ",
					mProjectFile.c_str(), e.what() );
		return false;
	}

	for ( const auto& build : j.items() ) {
		ProjectBuild b( build.key(), mProjectRoot );
		const auto& buildObj = build.value();

		if ( buildObj.contains( "config" ) && buildObj["config"].is_object() ) {
			b.mConfig.clearSysEnv = buildObj.value( "clear_sys_env", false );
		}

		if ( buildObj.contains( "var" ) && buildObj["var"].is_object() ) {
			const auto& vars = buildObj["var"];
			for ( const auto& var : vars.items() )
				b.mVars[var.key()] = var.value();
		}

		if ( buildObj.contains( "env" ) && buildObj["env"].is_object() ) {
			const auto& vars = buildObj["env"];
			for ( const auto& var : vars.items() )
				b.mEnvs[var.key()] = var.value();
		}

		if ( buildObj.contains( "build" ) && buildObj["build"].is_array() ) {
			const auto& buildArray = buildObj["build"];
			for ( const auto& step : buildArray ) {
				ProjectBuildStep bstep;
				bstep.cmd = step.value( "command", "" );
				bstep.args = step.value( "args", "" );
				bstep.workingDir = step.value( "working_dir", "" );
				b.mBuild.emplace_back( std::move( bstep ) );
			}
		}

		if ( buildObj.contains( "clean" ) && buildObj["clean"].is_array() ) {
			const auto& buildArray = buildObj["clean"];
			for ( const auto& step : buildArray ) {
				ProjectBuildStep bstep;
				bstep.cmd = step.value( "command", "" );
				bstep.args = step.value( "args", "" );
				bstep.workingDir = step.value( "working_dir", "" );
				b.mClean.emplace_back( std::move( bstep ) );
			}
		}

		if ( buildObj.contains( "output_parser" ) && buildObj["output_parser"].is_object() ) {
			const auto& op = buildObj["output_parser"];

			ProjectBuildOutputParser outputParser;

			for ( const auto& op : op.items() ) {
				if ( op.key() == "config" ) {
					const auto& config = op.value();
					outputParser.mRelativeFilePaths = config.value( "output_parser", true );
				} else {
					auto typeStr = String::toLower( op.key() );

					if ( !isValidType( typeStr ) )
						continue;

					const auto& ptrnCfg = op.value();
					ProjectBuildOutputParserConfig opc;
					opc.type = outputParserType( typeStr );
					opc.pattern = ptrnCfg.value( "pattern", "" );

					if ( ptrnCfg.contains( "pattern_order" ) ) {
						const auto& po = ptrnCfg["pattern_order"];
						if ( po.contains( "line" ) && po["line"].is_number() )
							opc.patternOrder.line = po["line"].get<int>();
						if ( po.contains( "col" ) && po["col"].is_number() )
							opc.patternOrder.col = po["col"].get<int>();
						if ( po.contains( "message" ) && po["message"].is_number() )
							opc.patternOrder.message = po["message"].get<int>();
						if ( po.contains( "file" ) && po["file"].is_number() )
							opc.patternOrder.file = po["file"].get<int>();
					}

					outputParser.mConfig.emplace_back( std::move( opc ) );
				}
			}
		}

		b.replaceVars();

		mBuilds.insert( { build.key(), std::move( b ) } );
	}

	mLoaded = true;
	return true;
}

void ProjectBuildManager::run( const std::string& buildName ) {
	if ( !mLoaded )
		return;
}

} // namespace ecode
