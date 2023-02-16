#include "lspclientplugin.hpp"
#include <eepp/system/filesystem.hpp>
#include <eepp/system/lock.hpp>
#include <eepp/system/luapattern.hpp>
#include <eepp/ui/tools/uicodeeditorsplitter.hpp>
#include <eepp/ui/uiscenenode.hpp>
#include <eepp/ui/uitooltip.hpp>
#include <eepp/window/input.hpp>
#include <eepp/window/window.hpp>
#include <nlohmann/json.hpp>

using namespace EE::Window;
using json = nlohmann::json;

namespace ecode {

UICodeEditorPlugin* LSPClientPlugin::New( PluginManager* pluginManager ) {
	return eeNew( LSPClientPlugin, ( pluginManager, false ) );
}

UICodeEditorPlugin* LSPClientPlugin::NewSync( PluginManager* pluginManager ) {
	return eeNew( LSPClientPlugin, ( pluginManager, true ) );
}

LSPClientPlugin::LSPClientPlugin( PluginManager* pluginManager, bool sync ) :
	mManager( pluginManager ), mThreadPool( pluginManager->getThreadPool() ) {
	if ( sync ) {
		load( pluginManager );
	} else {
		mThreadPool->run( [&, pluginManager] { load( pluginManager ); }, [] {} );
	}
}

LSPClientPlugin::~LSPClientPlugin() {
	mClosing = true;
	mManager->unsubscribeMessages( this );
	Lock l( mDocMutex );
	for ( const auto& editor : mEditors ) {
		for ( auto& kb : mKeyBindings ) {
			editor.first->getKeyBindings().removeCommandKeybind( kb.first );
			if ( editor.first->hasDocument() )
				editor.first->getDocument().removeCommand( kb.first );
		}
		editor.first->unregisterPlugin( this );
	}
	if ( nullptr == mManager->getSplitter() )
		return;
	for ( const auto& editor : mEditorsTags ) {
		if ( mManager->getSplitter()->editorExists( editor.first ) ) {
			for ( const auto& tag : editor.second )
				editor.first->removeActionsByTag( tag );
		}
	}
}

void LSPClientPlugin::update( UICodeEditor* ) {
	mClientManager.updateDirty();
}

struct LSPPositionAndServer {
	LSPPosition loc;
	LSPClientServer* server{ nullptr };
};

struct LSPURIAndServer {
	URI uri;
	LSPClientServer* server{ nullptr };
};

static LSPClientServer* getServerURIFromURI( LSPClientServerManager& manager, const json& data ) {
	URI uri( data["uri"] );
	return manager.getOneLSPClientServer( uri );
}

static LSPURIAndServer getServerURIFromTextDocumentURI( LSPClientServerManager& manager,
														const json& data ) {
	if ( !data.contains( "textDocument" ) || !data["textDocument"].contains( "uri" ) )
		return {};
	URI uri( data["textDocument"]["uri"] );
	return { uri, manager.getOneLSPClientServer( uri ) };
}

static void sanitizeCommand( std::string& cmd ) {
	String::replaceAll( cmd, "$NPROC", String::toString( Sys::getCPUCount() ) );
}

LSPPositionAndServer getLSPLocationFromJSON( LSPClientServerManager& manager, const json& data ) {
	if ( !data.contains( "uri" ) || !data.contains( "position" ) )
		return {};

	TextPosition position( LSPConverter::fromJSON( data["position"] ) );
	if ( !position.isValid() )
		return {};

	URI uri( data["uri"] );
	auto server = manager.getOneLSPClientServer( uri );
	if ( !server )
		return {};
	return { { uri, position }, server };
}

PluginRequestHandle LSPClientPlugin::processCodeCompletionRequest( const PluginMessage& msg ) {
	if ( !msg.isRequest() || !msg.isJSON() )
		return {};

	auto res = getLSPLocationFromJSON( mClientManager, msg.asJSON() );
	if ( !res.server )
		return {};

	auto ret = res.server->documentCompletion(
		res.loc.uri, res.loc.pos,
		[&]( const PluginIDType& id, const LSPCompletionList& completionList ) {
			mManager->sendResponse( this, PluginMessageType::CodeCompletion,
									PluginMessageFormat::CodeCompletion, &completionList, id );
		} );

	return std::move( ret );
}

PluginRequestHandle LSPClientPlugin::processSignatureHelpRequest( const PluginMessage& msg ) {
	if ( !msg.isRequest() || !msg.isJSON() )
		return {};

	auto res = getLSPLocationFromJSON( mClientManager, msg.asJSON() );
	if ( !res.server )
		return {};

	auto ret = res.server->signatureHelp(
		res.loc.uri, res.loc.pos, [&]( const PluginIDType& id, const LSPSignatureHelp& data ) {
			mManager->sendResponse( this, PluginMessageType::SignatureHelp,
									PluginMessageFormat::SignatureHelp, &data, id );
		} );

	return std::move( ret );
}

PluginRequestHandle LSPClientPlugin::processDocumentFormatting( const PluginMessage& msg ) {
	if ( !msg.isBroadcast() || !msg.isJSON() )
		return {};

	auto server = getServerURIFromTextDocumentURI( mClientManager, msg.asJSON() );
	if ( !server.server )
		return {};

	if ( !msg.asJSON().contains( "options" ) )
		return {};

	auto ret = server.server->documentFormatting(
		server.uri, msg.asJSON()["options"],
		[&, server]( const PluginIDType&, const std::vector<LSPTextEdit>& edits ) {
			processDocumentFormattingResponse( server.uri, edits );
		} );

	return std::move( ret );
}

void LSPClientPlugin::processDocumentFormattingResponse( const URI& uri,
														 const std::vector<LSPTextEdit>& edits ) {
	auto doc = mManager->getSplitter()->findDocFromPath( uri.getPath() );
	if ( !doc )
		return;

	for ( const auto& edit : edits )
		if ( !edit.range.isValid() || !doc->isValidRange( edit.range ) )
			return;

	TextRanges ranges = doc->getSelections();

	for ( const auto& edit : edits ) {
		doc->setSelection( edit.range );
		if ( edit.text.empty() ) {
			doc->deleteSelection( 0 );
		} else {
			if ( edit.range.hasSelection() )
				doc->deleteTo( 0, 0 );
			doc->setSelection( 0,
							   doc->insert( 0, doc->getSelectionIndex( 0 ).start(), edit.text ) );
		}
	}

	doc->setSelection( ranges );
}

bool LSPClientPlugin::editorExists( UICodeEditor* editor ) {
	return mManager->getSplitter()->editorExists( editor );
}

PluginRequestHandle LSPClientPlugin::processCancelRequest( const PluginMessage& msg ) {
	if ( !msg.isBroadcast() || !msg.isJSON() )
		return {};

	auto server = getServerURIFromURI( mClientManager, msg.asJSON() );
	if ( !server )
		return {};

	return server->cancel( LSPClientServer::getID( msg.asJSON() ) );
}

PluginRequestHandle LSPClientPlugin::processMessage( const PluginMessage& msg ) {
	switch ( msg.type ) {
		case PluginMessageType::WorkspaceFolderChanged: {
			mClientManager.didChangeWorkspaceFolders( msg.asJSON()["folder"] );
			break;
		}
		case PluginMessageType::CodeCompletion: {
			auto ret = processCodeCompletionRequest( msg );
			if ( !ret.isEmpty() )
				return ret;
			break;
		}
		case PluginMessageType::SignatureHelp: {
			auto ret = processSignatureHelpRequest( msg );
			if ( !ret.isEmpty() )
				return ret;
			break;
		}
		case PluginMessageType::DocumentFormatting: {
			auto ret = processDocumentFormatting( msg );
			if ( !ret.isEmpty() )
				return ret;
			break;
		}
		case PluginMessageType::LanguageServerCapabilities: {
			if ( msg.isRequest() && msg.isJSON() ) {
				const auto& data = msg.asJSON();
				if ( !data.contains( "language" ) ) {
					return {};
				}
				auto server = mClientManager.getOneLSPClientServer( msg.asJSON()["language"] );
				if ( server ) {
					mManager->sendBroadcast( this, PluginMessageType::LanguageServerCapabilities,
											 PluginMessageFormat::LanguageServerCapabilities,
											 &server->getCapabilities() );
					return PluginRequestHandle::broadcast();
				}
			}
			break;
		}
		case PluginMessageType::CancelRequest: {
			processCancelRequest( msg );
			break;
		}
		default:
			break;
	}
	return PluginRequestHandle::empty();
}

void LSPClientPlugin::load( PluginManager* pluginManager ) {
	pluginManager->subscribeMessages( this, [&]( const auto& notification ) -> PluginRequestHandle {
		return processMessage( notification );
	} );
	std::vector<std::string> paths;
	std::string path( pluginManager->getResourcesPath() + "plugins/lspclient.json" );
	if ( FileSystem::fileExists( path ) )
		paths.emplace_back( path );
	path = pluginManager->getPluginsPath() + "lspclient.json";
	if ( FileSystem::fileExists( path ) ||
		 FileSystem::fileWrite(
			 path, "{\n  \"config\":{},\n  \"keybindings\":{},\n  \"servers\":[]\n}\n" ) ) {
		mConfigPath = path;
		paths.emplace_back( path );
	}
	if ( paths.empty() )
		return;

	std::vector<LSPDefinition> lsps;

	for ( const auto& path : paths ) {
		try {
			loadLSPConfig( lsps, path, mConfigPath == path );
		} catch ( const json::exception& e ) {
			Log::error( "Parsing LSP \"%s\" failed:\n%s", path.c_str(), e.what() );
		}
	}

	mClientManager.load( this, pluginManager, std::move( lsps ) );

	mReady = mClientManager.lspCount() > 0;
	for ( const auto& doc : mDelayedDocs )
		if ( mDocs.find( doc.first ) != mDocs.end() )
			mClientManager.tryRunServer( doc.second );
	mDelayedDocs.clear();
	if ( mReady )
		fireReadyCbs();
}

void LSPClientPlugin::loadLSPConfig( std::vector<LSPDefinition>& lsps, const std::string& path,
									 bool updateConfigFile ) {
	std::string data;
	if ( !FileSystem::fileGet( path, data ) )
		return;
	json j;
	try {
		j = json::parse( data, nullptr, true, true );
	} catch ( const json::exception& e ) {
		Log::error( "LSPClientPlugin::loadLSPConfig - Error parsing LSP config from "
					"path %s, error: ",
					path.c_str(), e.what() );
		return;
	}

	if ( j.contains( "config" ) ) {
		auto& config = j["config"];
		if ( config.contains( "hover_delay" ) )
			setHoverDelay( Time::fromString( config["hover_delay"].get<std::string>() ) );
		else if ( updateConfigFile )
			config["hover_delay"] = getHoverDelay().toString();

		if ( config.contains( "server_close_after_idle_time" ) )
			mClientManager.setLSPDecayTime(
				Time::fromString( config["server_close_after_idle_time"].get<std::string>() ) );
		else if ( updateConfigFile )
			config["server_close_after_idle_time"] = mClientManager.getLSPDecayTime().toString();
	}

	if ( mKeyBindings.empty() ) {
		mKeyBindings["lsp-go-to-definition"] = "f2";
		mKeyBindings["lsp-symbol-info"] = "f1";
	}

	if ( j.contains( "keybindings" ) ) {
		auto& kb = j["keybindings"];
		auto list = { "lsp-go-to-definition",	  "lsp-go-to-declaration",
					  "lsp-go-to-implementation", "lsp-go-to-type-definition",
					  "lsp-switch-header-source", "lsp-symbol-info",
					  "lsp-symbol-references",	  "lsp-memory-usage" };
		for ( const auto& key : list ) {
			if ( kb.contains( key ) ) {
				if ( !kb[key].empty() )
					mKeyBindings[key] = kb[key];
			} else if ( updateConfigFile )
				kb[key] = mKeyBindings[key];
		}
	}

	if ( updateConfigFile ) {
		FileSystem::fileWrite( path, j.dump( 2 ) );
	}

	if ( !j.contains( "servers" ) )
		return;
	auto& servers = j["servers"];

	for ( auto& obj : servers ) {
		bool lspOverwritten = false;
		// Allow disabling an LSP by redeclaring it in the user configuration file.
		if ( updateConfigFile && ( obj.contains( "name" ) || obj.contains( "use" ) ) &&
			 obj.contains( "disabled" ) && obj.at( "disabled" ).is_boolean() ) {
			std::string name = obj.contains( "name" ) ? obj["name"] : obj["use"];
			for ( auto& lspD : lsps ) {
				if ( lspD.name == name ) {
					lspD.disabled = obj["disabled"].get<bool>();
					lspOverwritten = true;
				}
			}
		}

		// Allow overriding the command for already defined LSP
		// And allow adding parameters to the already defined LSP
		if ( updateConfigFile && ( obj.contains( "name" ) || obj.contains( "use" ) ) &&
			 ( ( obj.contains( "command" ) && obj.at( "command" ).is_string() ) ||
			   ( obj.contains( "command_parameters" ) &&
				 obj.at( "command_parameters" ).is_string() ) ) ) {
			for ( auto& lspR : lsps ) {
				std::string name = obj.contains( "name" ) ? obj["name"] : obj["use"];
				if ( lspR.name == name ) {
					lspOverwritten = true;
					if ( !obj.value( "command", "" ).empty() ) {
						lspR.command = obj.value( "command", "" );
						sanitizeCommand( lspR.command );
					}
					if ( !obj.value( "command_parameters", "" ).empty() ) {
						std::string cmdParam( obj.value( "command_parameters", "" ) );
						if ( !cmdParam.empty() && cmdParam.front() != ' ' )
							cmdParam = " " + cmdParam;
						lspR.commandParameters += cmdParam;
						sanitizeCommand( lspR.commandParameters );
					}
				}
			}
		}

		if ( lspOverwritten )
			continue;

		if ( !obj.contains( "language" ) || !obj.contains( "file_patterns" ) ) {
			Log::warning( "LSP server without language or file_patterns, ignored..." );
			continue;
		}

		if ( !obj.contains( "use" ) && !( obj.contains( "command" ) && obj.contains( "name" ) ) ) {
			Log::warning( "LSP server without name+command or use, ignored..." );
			continue;
		}

		LSPDefinition lsp;
		if ( obj.contains( "use" ) ) {
			std::string use = obj["use"];
			bool foundTlsp = false;
			for ( const auto& tlsp : lsps ) {
				if ( tlsp.name == use ) {
					lsp.language = obj["language"];
					foundTlsp = true;
					lsp.command = tlsp.command;
					lsp.name = tlsp.name;
					lsp.rootIndicationFileNames = tlsp.rootIndicationFileNames;
					lsp.url = tlsp.url;
					lsp.initializationOptions = tlsp.initializationOptions;
					break;
				}
			}
			if ( !foundTlsp ) {
				Log::warning( "LSP server trying to use an undeclared LSP. Father LSP must be "
							  "declared first." );
				continue;
			}
		} else {
			lsp.language = obj["language"];
			lsp.command = obj["command"];
			lsp.name = obj["name"];
		}

		lsp.url = obj.value( "url", "" );
		lsp.commandParameters = obj.value( "command_parameters", lsp.commandParameters );
		if ( obj.contains( "initializationOptions" ) )
			lsp.initializationOptions = obj["initializationOptions"];

		auto& fp = obj["file_patterns"];

		for ( auto& pattern : fp )
			lsp.filePatterns.push_back( pattern.get<std::string>() );

		if ( obj.contains( "rootIndicationFileNames" ) ) {
			lsp.rootIndicationFileNames.clear();
			auto& fnms = obj["rootIndicationFileNames"];
			for ( auto& fn : fnms )
				lsp.rootIndicationFileNames.push_back( fn );
		}

		sanitizeCommand( lsp.command );
		sanitizeCommand( lsp.commandParameters );

		// If the file pattern is repeated, we will overwrite the previous LSP.
		// The previous LSP should be the "default" LSP that comes with ecode.
		size_t pos = lspFilePatternPosition( lsps, lsp.filePatterns );
		if ( pos != std::string::npos ) {
			lsps[pos] = lsp;
		} else {
			lsps.emplace_back( std::move( lsp ) );
		}
	}
}

size_t LSPClientPlugin::lspFilePatternPosition( const std::vector<LSPDefinition>& lsps,
												const std::vector<std::string>& patterns ) {
	for ( size_t i = 0; i < lsps.size(); ++i ) {
		for ( const std::string& filePattern : lsps[i].filePatterns ) {
			for ( const std::string& pattern : patterns ) {
				if ( filePattern == pattern ) {
					return i;
				}
			}
		}
	}
	return std::string::npos;
}

void LSPClientPlugin::onRegister( UICodeEditor* editor ) {
	Lock l( mDocMutex );
	mDocs.insert( editor->getDocumentRef().get() );

	for ( auto& kb : mKeyBindings ) {
		if ( !kb.second.empty() )
			editor->getKeyBindings().addKeybindString( kb.second, kb.first );
	}

	if ( editor->hasDocument() ) {
		auto& doc = editor->getDocument();

		doc.setCommand( "lsp-go-to-definition", [&, editor]() {
			mClientManager.getAndGoToLocation( editor->getDocumentRef(),
											   "textDocument/definition" );
		} );

		doc.setCommand( "lsp-go-to-declaration", [&, editor]() {
			mClientManager.getAndGoToLocation( editor->getDocumentRef(),
											   "textDocument/declaration" );
		} );

		doc.setCommand( "lsp-go-to-implementation", [&, editor]() {
			mClientManager.getAndGoToLocation( editor->getDocumentRef(),
											   "textDocument/implementation" );
		} );

		doc.setCommand( "lsp-go-to-type-definition", [&, editor]() {
			mClientManager.getAndGoToLocation( editor->getDocumentRef(),
											   "textDocument/typeDefinition" );
		} );

		doc.setCommand( "lsp-switch-header-source",
						[&, editor]() { switchSourceHeader( editor ); } );

		doc.setCommand( "lsp-symbol-info", [&, editor]() { getSymbolInfo( editor ); } );

		doc.setCommand( "lsp-symbol-references", [&, editor] {
			mClientManager.getSymbolReferences( editor->getDocumentRef() );
		} );

		doc.setCommand( "lsp-memory-usage",
						[&, editor] { mClientManager.memoryUsage( editor->getDocumentRef() ); } );
	}

	std::vector<Uint32> listeners;

	listeners.push_back(
		editor->addEventListener( Event::OnDocumentLoaded, [&, editor]( const Event* ) {
			mClientManager.run( editor->getDocumentRef() );
		} ) );

	listeners.push_back(
		editor->addEventListener( Event::OnCursorPosChange, [&, editor]( const Event* ) {
			if ( mSymbolInfoShowing )
				hideTooltip( editor );
		} ) );

	mEditors.insert( { editor, listeners } );
	mEditorsTags.insert( { editor, {} } );
	mEditorDocs[editor] = editor->getDocumentRef().get();

	if ( mReady && editor->hasDocument() && editor->getDocument().hasFilepath() )
		mClientManager.run( editor->getDocumentRef() );
	if ( !mReady )
		mDelayedDocs[&editor->getDocument()] = editor->getDocumentRef();
}

void LSPClientPlugin::switchSourceHeader( UICodeEditor* editor ) {
	auto* server = mClientManager.getOneLSPClientServer( editor );
	if ( server )
		server->switchSourceHeader( editor->getDocument().getURI() );
}

void LSPClientPlugin::getSymbolInfo( UICodeEditor* editor ) {
	auto server = mClientManager.getOneLSPClientServer( editor );
	if ( server == nullptr )
		return;
	server->documentHover(
		editor->getDocument().getURI(), editor->getDocument().getSelection().start(),
		[&, editor]( const Int64&, const LSPHover& resp ) {
			if ( !resp.contents.empty() && !resp.contents[0].value.empty() ) {
				editor->runOnMainThread( [editor, resp, this]() {
					mSymbolInfoShowing = true;
					displayTooltip(
						editor, resp,
						editor->getScreenPosition( editor->getDocument().getSelection().start() )
							.getPosition() );
				} );
			}
		} );
}

void LSPClientPlugin::onUnregister( UICodeEditor* editor ) {
	for ( auto& kb : mKeyBindings ) {
		editor->getKeyBindings().removeCommandKeybind( kb.first );
		if ( editor->hasDocument() )
			editor->getDocument().removeCommand( kb.first );
	}

	if ( mClosing )
		return;
	Lock l( mDocMutex );
	TextDocument* doc = mEditorDocs[editor];
	auto& cbs = mEditors[editor];
	for ( auto listener : cbs )
		editor->removeEventListener( listener );
	mEditors.erase( editor );
	mEditorsTags.erase( editor );
	mEditorDocs.erase( editor );
	for ( auto& editor : mEditorDocs )
		if ( editor.second == doc )
			return;
	mDocs.erase( doc );
}

PluginManager* LSPClientPlugin::getManager() const {
	return mManager;
}

bool LSPClientPlugin::onCreateContextMenu( UICodeEditor* editor, UIPopUpMenu* menu,
										   const Vector2i& /*position*/, const Uint32& /*flags*/ ) {
	auto* server = mClientManager.getOneLSPClientServer( editor );
	if ( !server )
		return false;

	menu->addSeparator();

	auto addFn = [this, editor, menu]( const std::string& txtKey, const std::string& txtVal ) {
		menu->add( editor->getUISceneNode()->i18n( txtKey, txtVal ), nullptr,
				   KeyBindings::keybindFormat( mKeyBindings[txtKey] ) )
			->setId( txtKey );
	};
	auto& cap = server->getCapabilities();

	addFn( "lsp-symbol-info", "Symbol Info" );

	if ( cap.definitionProvider )
		addFn( "lsp-go-to-definition", "Go To Definition" );

	if ( cap.declarationProvider )
		addFn( "lsp-go-to-declaration", "Go To Declaration" );

	if ( cap.typeDefinitionProvider )
		addFn( "lsp-go-to-type-definition", "Go To Type Definition" );

	if ( cap.implementationProvider )
		addFn( "lsp-go-to-implementation", "Go To Implementation" );

	if ( cap.referencesProvider )
		addFn( "lsp-symbol-references", "Find References to Symbol Under Cursor" );

	if ( server->getDefinition().language == "cpp" || server->getDefinition().language == "c" )
		addFn( "lsp-switch-header-source", "Switch Header/Source" );

#ifdef EE_DEBUG
	if ( server->getDefinition().name == "clangd" )
		addFn( "lsp-memory-usage", "LSP Memory Usage" );
#endif

	return false;
}

void LSPClientPlugin::hideTooltip( UICodeEditor* editor ) {
	mSymbolInfoShowing = false;
	UITooltip* tooltip = nullptr;
	if ( editor && ( tooltip = editor->getTooltip() ) && tooltip->isVisible() ) {
		editor->setTooltipText( "" );
		tooltip->hide();
		// Restore old tooltip state
		tooltip->setFontStyle( mOldTextStyle );
		tooltip->setHorizontalAlign( mOldTextAlign );
		tooltip->setUsingCustomStyling( mOldUsingCustomStyling );
		tooltip->setDontAutoHideOnMouseMove( mOldDontAutoHideOnMouseMove );
	}
}

TextPosition currentMouseTextPosition( UICodeEditor* editor ) {
	return editor->resolveScreenPosition(
		editor->getUISceneNode()->getWindow()->getInput()->getMousePosf() );
}

void LSPClientPlugin::tryHideTooltip( UICodeEditor* editor, const Vector2i& position ) {
	TextPosition cursorPosition = editor->resolveScreenPosition( position.asFloat() );
	if ( !mCurrentHover.range.isValid() ||
		 ( mCurrentHover.range.isValid() && !mCurrentHover.range.contains( cursorPosition ) ) )
		hideTooltip( editor );
}

void LSPClientPlugin::displayTooltip( UICodeEditor* editor, const LSPHover& resp,
									  const Vector2f& position ) {
	editor->setTooltipText( resp.contents[0].value );
	// HACK: Gets the old font style to restore it when the tooltip is hidden
	UITooltip* tooltip = editor->getTooltip();
	if ( tooltip == nullptr )
		return;
	mOldTextStyle = tooltip->getFontStyle();
	mOldTextAlign = tooltip->getHorizontalAlign();
	mOldDontAutoHideOnMouseMove = tooltip->dontAutoHideOnMouseMove();
	mOldUsingCustomStyling = tooltip->getUsingCustomStyling();
	tooltip->setHorizontalAlign( UI_HALIGN_LEFT );
	tooltip->setPixelsPosition( tooltip->getTooltipPosition( position ) );
	tooltip->setDontAutoHideOnMouseMove( true );
	tooltip->setUsingCustomStyling( true );
	tooltip->setFontStyle( Text::Regular );

	const auto& syntaxDef = resp.contents[0].kind == LSPMarkupKind::MarkDown
								? SyntaxDefinitionManager::instance()->getByLSPName( "markdown" )
								: editor->getSyntaxDefinition();

	SyntaxTokenizer::tokenizeText( syntaxDef, editor->getColorScheme(), *tooltip->getTextCache(), 0,
								   0xFFFFFFFF, true, "\n\t " );

	tooltip->notifyTextChangedFromTextCache();

	if ( editor->hasFocus() && !tooltip->isVisible() )
		tooltip->show();
}

void LSPClientPlugin::tryDisplayTooltip( UICodeEditor* editor, const LSPHover& resp,
										 const Vector2i& position ) {
	TextPosition startCursorPosition = editor->resolveScreenPosition( position.asFloat() );
	TextPosition currentMousePosition = currentMouseTextPosition( editor );
	if ( startCursorPosition != currentMousePosition ||
		 !( !resp.contents.empty() && !resp.contents[0].value.empty() &&
			( ( resp.range.isValid() && resp.range.contains( startCursorPosition ) ) ||
			  !resp.range.isValid() ) ) )
		return;
	mCurrentHover = resp;
	displayTooltip( editor, resp, position.asFloat() );
}

bool LSPClientPlugin::onMouseMove( UICodeEditor* editor, const Vector2i& position,
								   const Uint32& flags ) {
	if ( flags != 0 ) {
		tryHideTooltip( editor, position );
		return false;
	}
	String::HashType tag = String::hash( editor->getDocument().getFilePath() );
	editor->removeActionsByTag( tag );
	mEditorsTags[editor].insert( tag );
	editor->runOnMainThread(
		[&, editor, position, tag]() {
			mEditorsTags[editor].erase( tag );
			mThreadPool->run( [&, editor, position]() {
				if ( !editorExists( editor ) )
					return;
				auto server = mClientManager.getOneLSPClientServer( editor );
				if ( server == nullptr )
					return;
				server->documentHover(
					editor->getDocument().getURI(), currentMouseTextPosition( editor ),
					[&, editor, position]( const Int64&, const LSPHover& resp ) {
						if ( editorExists( editor ) && !resp.contents.empty() &&
							 !resp.contents[0].value.empty() ) {
							editor->runOnMainThread( [editor, resp, position, this]() {
								tryDisplayTooltip( editor, resp, position );
							} );
						}
					} );
			} );
		},
		mHoverDelay, tag );
	tryHideTooltip( editor, position );
	return editor->getTooltip() && editor->getTooltip()->isVisible();
}

void LSPClientPlugin::onFocusLoss( UICodeEditor* editor ) {
	hideTooltip( editor );
}

bool LSPClientPlugin::onKeyDown( UICodeEditor* editor, const KeyEvent& event ) {
	if ( event.getSanitizedMod() == 0 && event.getKeyCode() == KEY_ESCAPE && editor->getTooltip() &&
		 editor->getTooltip()->isVisible() ) {
		hideTooltip( editor );
	}

	return false;
}

const Time& LSPClientPlugin::getHoverDelay() const {
	return mHoverDelay;
}

void LSPClientPlugin::setHoverDelay( const Time& hoverDelay ) {
	mHoverDelay = hoverDelay;
}

const LSPClientServerManager& LSPClientPlugin::getClientManager() const {
	return mClientManager;
}

bool LSPClientPlugin::hasFileConfig() {
	return !mConfigPath.empty();
}

std::string LSPClientPlugin::getFileConfigPath() {
	return mConfigPath;
}

} // namespace ecode
