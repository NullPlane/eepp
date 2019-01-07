#include <eepp/ee.hpp>
#include <efsw/efsw.hpp>
#include <pugixml/pugixml.hpp>

/**
This is a real time visual editor for the UI module.
The layout files can be edited with any editor, and the layout changes can be seen live with this editor.
So this is a layout preview app.
The layout is updated every time the layout file is modified by the user. So you'll need to save the file in your editor to see the changes.
This was done in a rush for a personal project ( hence the horrendous code ), but it's quite useful and functional.
Project files are created by hand for the moment, and they shuld look like this one:

<uiproject>
	<basepath>/optional/project/root/path</basepath>
	<font>
		<path>font</path>
	</font>
	<drawable>
		<path>drawable</path>
		<path>background</path>
	</drawable>
	<widget>
		<customWidget name="ScreenGame" replacement="RelativeLayout" />
	</widget>
	<layout width="1920" height="1080">
		<path>layout</path>
	</layout>
</uiproject>

basepath is optional, otherwise it will take the base path from the xml file itself ( it xml is in /home/project.xml, basepath it going to be /home )

Layout width and height are the default/target window size for the project.

"path"s could be explicit files or directories.

customWidget are defined in the case you use special widgets in your application, you'll need to indicate a valid replacement to be able to edit the file.
*/

EE::Window::Window * window = NULL;
UIMessageBox * MsgBox = NULL;
efsw::FileWatcher * fileWatcher = NULL;
UITheme * theme = NULL;
UIWidget * uiContainer = NULL;
UIWinMenu * uiWinMenu = NULL;
UISceneNode * uiSceneNode = NULL;
UISceneNode * appUiSceneNode = NULL;
std::string currentLayout;
std::string currentStyleSheet;
bool updateLayout = false;
bool updateStyleSheet = false;
Clock waitClock;
Clock cssWaitClock;
efsw::WatchID watch = 0;
std::map<std::string, std::string> widgetRegistered;
std::string basePath;

Vector2i mousePos;
Clock mouseClock;

std::map<std::string,std::string> layouts;
std::vector<std::string> recentProjects;
IniFile ini;
Uint32 recentProjectEventClickId = 0xFFFFFFFF;

std::map<Uint32,TextureRegion*> imagesLoaded;
std::map<Font*,std::string> fontsLoaded;

void closeProject();
void updateRecentProjects();
void loadProject(std::string projectPath);

void loadConfig() {
	std::string path( Sys::getConfigPath( "eepp-uieditor" ) );
	if ( !FileSystem::fileExists( path ) ) FileSystem::makeDir( path );
	FileSystem::dirPathAddSlashAtEnd( path );
	path += "config.ini";

	ini.loadFromFile( path );
	ini.readFile();

	std::string recent = ini.getValue( "UIEDITOR", "recentfiles", "" );
	std::vector<std::string> files = String::split( recent, ';' );

	recentProjects = files;
}

void saveConfig() {
	std::string files = String::join( recentProjects, ';' );

	ini.setValue( "UIEDITOR", "recentfiles", files );
	ini.writeFile();
}

class UpdateListener : public efsw::FileWatchListener {
	public:
		UpdateListener() {}

		void handleFileAction( efsw::WatchID, const std::string& dir, const std::string& filename, efsw::Action action, std::string ) {
			if ( action == efsw::Actions::Modified ) {
				if ( dir + filename == currentLayout ) {
					updateLayout = true;
					waitClock.restart();
				} else if ( dir + filename == currentStyleSheet ) {
					updateStyleSheet = true;
					cssWaitClock.restart();
				}
			}
		}
};

UpdateListener * listener = NULL;

void unloadImages() {
	for ( auto it = imagesLoaded.begin(); it != imagesLoaded.end(); ++it ) {
		GlobalTextureAtlas::instance()->remove( it->second );
		TextureFactory::instance()->remove( it->first );
	}

	imagesLoaded.clear();
}

void unloadFonts() {
	for ( auto it = fontsLoaded.begin(); it != fontsLoaded.end(); ++it ) {
		FontManager::instance()->remove( it->first );
	}

	fontsLoaded.clear();
}

static bool isFont( const std::string& path ) {
	std::string ext = FileSystem::fileExtension( path );
	return ext == "ttf" || ext == "otf" || ext == "wolff";
}

static bool isXML( const std::string& path ) {
	return FileSystem::fileExtension( path ) == "xml";
}

static void loadImage( std::string path ) {
	std::string filename( FileSystem::fileRemoveExtension( FileSystem::fileNameFromPath( path ) ) );

	Uint32 texId = TextureFactory::instance()->loadFromFile( path );

	TextureRegion * texRegion = GlobalTextureAtlas::instance()->add( texId, filename );

	imagesLoaded[ texId ] = texRegion;
}

static void loadFont( std::string path ) {
	std::string filename( FileSystem::fileRemoveExtension( FileSystem::fileNameFromPath( path ) ) );
	FontTrueType * font = FontTrueType::New( filename );

	font->loadFromFile( path );

	fontsLoaded[ font ] = filename;
}

static void loadImagesFromFolder( std::string folderPath ) {
	std::vector<std::string> files = FileSystem::filesGetInPath( folderPath );

	FileSystem::dirPathAddSlashAtEnd( folderPath );

	for ( auto it = files.begin(); it != files.end(); ++it ) {
		if ( Image::isImageExtension( *it ) ) {
			loadImage( folderPath + (*it) );
		}
	}
}

static void loadFontsFromFolder( std::string folderPath ) {
	std::vector<std::string> files = FileSystem::filesGetInPath( folderPath );

	FileSystem::dirPathAddSlashAtEnd( folderPath );

	for ( auto it = files.begin(); it != files.end(); ++it ) {
		if ( isFont( *it ) ) {
			loadFont( folderPath + (*it) );
		}
	}
}

static void loadLayoutsFromFolder( std::string folderPath ) {
	std::vector<std::string> files = FileSystem::filesGetInPath( folderPath );

	FileSystem::dirPathAddSlashAtEnd( folderPath );

	for ( auto it = files.begin(); it != files.end(); ++it ) {
		if ( isXML( *it ) ) {
			layouts[ FileSystem::fileRemoveExtension( (*it) ) ] = ( folderPath + (*it) );
		}
	}
}

static void loadStyleSheet( std::string cssPath ) {
	CSS::StyleSheetParser parser;

	if ( NULL != uiSceneNode && parser.loadFromFile( cssPath ) ) {
		uiSceneNode->setStyleSheet( parser.getStyleSheet() );

		currentStyleSheet = cssPath;
	}
}

static void loadLayout( std::string file ) {
	if ( watch != 0 ) {
		fileWatcher->removeWatch( watch );
	}

	std::string folder( FileSystem::fileRemoveFileName( file ) );

	watch = fileWatcher->addWatch( folder, listener );

	uiContainer->childsCloseAll();

	uiSceneNode->loadLayoutFromFile( file, uiContainer );

	currentLayout = file;
}

static void refreshLayout() {
	if ( !currentLayout.empty() && FileSystem::fileExists( currentLayout ) && uiContainer != NULL ) {
		loadLayout( currentLayout );
	}

	updateLayout = false;
}

static void refreshStyleSheet() {
	if ( !currentStyleSheet.empty() && FileSystem::fileExists( currentStyleSheet ) && uiContainer != NULL ) {
		loadStyleSheet( currentStyleSheet );
	}

	updateStyleSheet = false;
}

void onRecentProjectClick( const Event * event ) {
	if ( !event->getNode()->isType( UI_TYPE_MENUITEM ) )
		return;

	const String& txt = reinterpret_cast<UIMenuItem*> ( event->getNode() )->getText();
	std::string path( txt.toUtf8() );

	if ( FileSystem::fileExists( path ) && !FileSystem::isDirectory( path ) ) {
		loadProject( path );
	}
}

void updateRecentProjects() {
	if ( NULL == uiWinMenu )
		return;

	UIPopUpMenu * fileMenu = uiWinMenu->getPopUpMenu( "File" );

	UINode * node = NULL;

	if ( NULL != fileMenu  && ( node = fileMenu->getItem( "Recent projects" ) ) ) {
		UIMenuSubMenu * uiMenuSubMenu = static_cast<UIMenuSubMenu*>( node );
		UIMenu * menu = uiMenuSubMenu->getSubMenu();

		menu->removeAll();

		for ( size_t i = 0; i < recentProjects.size(); i++ ) {
			menu->add( recentProjects[i] );
		}

		if ( 0xFFFFFFFF != recentProjectEventClickId ) {
			menu->removeEventListener( recentProjectEventClickId );
		}

		recentProjectEventClickId = menu->addEventListener( Event::OnItemClicked, cb::Make1( &onRecentProjectClick ) );
	}
}

void resizeCb(EE::Window::Window *) {
	Float scaleW = (Float)uiSceneNode->getSize().getWidth() / (Float)uiContainer->getSize().getWidth();
	Float scaleH = (Float)uiSceneNode->getSize().getHeight() / (Float)uiContainer->getSize().getHeight();

	uiContainer->setScale( scaleW < scaleH ? scaleW : scaleH );
	uiContainer->center();
}

void resizeWindowToLayout() {
	Sizef size( uiContainer->getSize() );
	Rect borderSize( window->getBorderSize() );
	Sizei displayMode = Engine::instance()->getDisplayManager()->getDisplayIndex( window->getCurrentDisplayIndex() )->getUsableBounds().getSize();
	displayMode.x = displayMode.x - borderSize.Left - borderSize.Right;
	displayMode.y = displayMode.y - borderSize.Top - borderSize.Bottom;

	Float scaleW = size.getWidth() > displayMode.getWidth() ? displayMode.getWidth() / size.getWidth() : 1.f;
	Float scaleH = size.getHeight() > displayMode.getHeight() ? displayMode.getHeight() / size.getHeight() : 1.f;
	Float scale = scaleW < scaleH ? scaleW : scaleH;

	window->setSize( (Uint32)( uiContainer->getSize().getWidth() * scale ), (Uint32)( uiContainer->getSize().getHeight() * scale ) );
	window->centerToDisplay();
}

static UIWidget * createWidget( std::string widgetName ) {
	return UIWidgetCreator::createFromName( widgetRegistered[ widgetName ] );
}

static std::string pathFix( std::string path ) {
	if ( !path.empty() && ( path.at(0) != '/' || !( Sys::getPlatform() == "Windows" && path.size() > 3 && path.at(1) != ':' ) ) ) {
		return basePath + path;
	}

	return path;
}

static void loadUITheme( std::string themePath ) {
	TextureAtlasLoader tgl( themePath );

	std::string name( FileSystem::fileRemoveExtension( FileSystem::fileNameFromPath( themePath ) ) );

	UITheme * uitheme = UITheme::loadFromTextureAtlas( UITheme::New( name, name ), TextureAtlasManager::instance()->getByName( name ) );

	UIThemeManager::instance()->setDefaultTheme( uitheme )->add( uitheme );
}

void onLayoutSelected( const Event * event ) {
	if ( !event->getNode()->isType( UI_TYPE_MENUCHECKBOX) )
		return;

	const String& txt = reinterpret_cast<UIMenuItem*> ( event->getNode() )->getText();

	UIPopUpMenu * uiLayoutsMenu;

	if ( ( uiLayoutsMenu = uiWinMenu->getPopUpMenu( "Layouts" ) ) && uiLayoutsMenu->getCount() > 0 ) {
		for ( size_t i = 0; i < uiLayoutsMenu->getCount(); i++ ) {
			UIMenuCheckBox * menuItem = static_cast<UIMenuCheckBox*>( uiLayoutsMenu->getItem( i ) );
			menuItem->setActive( false );
		}
	}

	UIMenuCheckBox * chk = static_cast<UIMenuCheckBox*>( event->getNode() );
	chk->setActive( true );

	std::map<std::string,std::string>::iterator it;

	if ( ( it = layouts.find( txt.toUtf8() ) ) != layouts.end() ) {
		loadLayout( it->second );
	}

}

void refreshLayoutList() {
	if ( NULL == uiWinMenu ) return;

	if ( layouts.size() > 0 ) {
		UIPopUpMenu * uiLayoutsMenu = NULL;

		if ( uiWinMenu->getButton( "Layouts" ) == NULL ) {
			uiLayoutsMenu = UIPopUpMenu::New();

			uiWinMenu->addMenuButton( "Layouts", uiLayoutsMenu );

			uiLayoutsMenu->addEventListener( Event::OnItemClicked, cb::Make1( &onLayoutSelected ) );
		} else {
			uiLayoutsMenu = uiWinMenu->getPopUpMenu( "Layouts" );
		}

		uiLayoutsMenu->removeAll();

		for ( auto it = layouts.begin(); it != layouts.end(); ++it ) {
			Uint32 idx = uiLayoutsMenu->addCheckBox( it->first );
			UIMenuCheckBox * chk = static_cast<UIMenuCheckBox*>( uiLayoutsMenu->getItem( idx ) );

			chk->setActive( currentLayout == it->second );
		}
	} else if ( uiWinMenu->getButton( "Layouts" ) == NULL ) {
		uiWinMenu->removeMenuButton( "Layouts" );
	}
}

static void loadProjectNodes( pugi::xml_node node ) {
	for ( pugi::xml_node resources = node; resources; resources = resources.next_sibling() ) {
		std::string name = String::toLower( resources.name() );

		if ( name == "uiproject" ) {
			pugi::xml_node basePathNode = resources.child( "basepath" );

			if ( !basePathNode.empty() ) {
				basePath = basePathNode.text().as_string();
			}

			pugi::xml_node fontNode = resources.child( "font" );

			if ( !fontNode.empty() ) {
				for ( pugi::xml_node pathNode = fontNode.child("path"); pathNode; pathNode= pathNode.next_sibling("path") ) {
					std::string fontPath( pathFix( pathNode.text().as_string() ) );

					if ( FileSystem::isDirectory( fontPath ) ) {
						loadFontsFromFolder( fontPath );
					} else if ( isFont( fontPath ) ) {
						loadFont( fontPath );
					}
				}
			}

			pugi::xml_node drawableNode = resources.child( "drawable" );

			if ( !drawableNode.empty() ) {
				for ( pugi::xml_node pathNode = drawableNode.child("path"); pathNode; pathNode= pathNode.next_sibling("path") ) {
					std::string drawablePath( pathFix( pathNode.text().as_string() ) );

					if ( FileSystem::isDirectory( drawablePath ) ) {
						loadImagesFromFolder( drawablePath );
					} else if ( Image::isImageExtension( drawablePath ) ) {
						loadImage( drawablePath );
					}
				}
			}

			pugi::xml_node widgetNode = resources.child( "widget" );

			if ( !widgetNode.empty() ) {
				for ( pugi::xml_node cwNode = widgetNode.child("customWidget"); cwNode; cwNode= cwNode.next_sibling("customWidget") ) {
					std::string wname( cwNode.attribute( "name" ).as_string() );
					std::string replacement( cwNode.attribute( "replacement" ).as_string() );
					widgetRegistered[ String::toLower( wname ) ] = replacement;
				}

				for ( auto it = widgetRegistered.begin(); it != widgetRegistered.end(); ++it ) {
					if ( !UIWidgetCreator::existsCustomWidgetCallback( it->first ) ) {
						UIWidgetCreator::addCustomWidgetCallback( it->first, cb::Make1( createWidget ) );
					}
				}
			}

			pugi::xml_node uiNode = resources.child( "uitheme" );

			if ( !uiNode.empty() ) {
				for ( pugi::xml_node uiThemeNode = uiNode; uiThemeNode; uiThemeNode = uiThemeNode.next_sibling("uitheme") ) {
					std::string uiThemePath( pathFix( uiThemeNode.text().as_string() ) );

					loadUITheme( uiThemePath );
				}
			}

			pugi::xml_node layoutNode = resources.child( "layout" );

			if ( !layoutNode.empty() ) {
				bool loaded = false;
				bool loadedSizedLayout = false;

				Float width = layoutNode.attribute( "width" ).as_float();
				Float height = layoutNode.attribute( "height" ).as_float();

				if ( 0.f != width && 0.f != height ) {
					uiContainer->setSize( width, height );
					resizeCb( window );
				}

				layouts.clear();

				for ( pugi::xml_node layNode = layoutNode.child("path"); layNode; layNode = layNode.next_sibling("path") ) {
					std::string layoutPath( pathFix( layNode.text().as_string() ) );

					if ( FileSystem::isDirectory( layoutPath ) ) {
						loadLayoutsFromFolder( layoutPath );
					} else if ( FileSystem::fileExists( layoutPath ) && isXML( layoutPath ) ) {
						layouts[ FileSystem::fileRemoveExtension( FileSystem::fileNameFromPath( layoutPath ) ) ] = layoutPath;

						if ( !loaded ) {
							loadLayout( layoutPath );
							loaded = true;
						}

						if ( width != 0 && height != 0 ) {
							loadedSizedLayout = true;
						}
					}
				}

				if ( layouts.size() > 0 && !loaded ) {
					loadLayout( layouts.begin()->second );

					if ( width != 0 && height != 0 ) {
						loadedSizedLayout = true;
					}
				}

				if ( loadedSizedLayout ) {
					resizeWindowToLayout();
				}
			}

			refreshLayoutList();
		}
	}
}

void loadProject( std::string projectPath ) {
	if ( FileSystem::fileExists( projectPath ) ) {
		closeProject();

		basePath = FileSystem::fileRemoveFileName( projectPath );

		pugi::xml_document doc;
		pugi::xml_parse_result result = doc.load_file( projectPath.c_str() );

		if ( result ) {
			loadProjectNodes( doc.first_child() );

			if ( recentProjects.size() > 0 && recentProjects[0] == projectPath )
				return;

			recentProjects.insert( recentProjects.begin(), projectPath );

			if ( recentProjects.size() > 10 ) {
				recentProjects.resize( 10 );
			}

			updateRecentProjects();
		} else {
			eePRINTL( "Error: Couldn't load UI Layout: %s", projectPath.c_str() );
			eePRINTL( "Error description: %s", result.description() );
			eePRINTL( "Error offset: %d", result.offset );
		}
	}
}

void closeProject() {
	currentLayout = "";
	currentStyleSheet = "";
	uiContainer->childsCloseAll();

	layouts.clear();

	refreshLayoutList();

	unloadFonts();
	unloadImages();
}

bool onCloseRequestCallback( EE::Window::Window * ) {
	UITheme * prevTheme = UIThemeManager::instance()->getDefaultTheme();
	UIThemeManager::instance()->setDefaultTheme( theme );

	MsgBox = UIMessageBox::New( MSGBOX_OKCANCEL, "Do you really want to close the current file?\nAll changes will be lost." );
	MsgBox->setTheme( theme );
	MsgBox->addEventListener( Event::MsgBoxConfirmClick, cb::Make1<void, const Event*>( []( const Event * ) { window->close(); } ) );
	MsgBox->addEventListener( Event::OnClose, cb::Make1<void, const Event*>( []( const Event * ) { MsgBox = NULL; } ) );
	MsgBox->setTitle( "Close Editor?" );
	MsgBox->center();
	MsgBox->show();

	UIThemeManager::instance()->setDefaultTheme( prevTheme );

	return false;
}

void mainLoop() {
	window->getInput()->update();

	if ( window->getInput()->isKeyUp( KEY_ESCAPE ) && NULL == MsgBox && onCloseRequestCallback( window ) ) {
		window->close();
	}

	if ( NULL != uiContainer && window->getInput()->isKeyUp( KEY_F1 ) ) {
		resizeWindowToLayout();
	}

	if ( mousePos != window->getInput()->getMousePos() ) {
		mousePos = window->getInput()->getMousePos();
		mouseClock.restart();

		if ( uiWinMenu->getAlpha() != 255 && uiWinMenu->getActionManager()->isEmpty() ) {
			uiWinMenu->runAction( Actions::Fade::New( uiWinMenu->getAlpha(), 255, Milliseconds(250) ) );
		}
	} else if ( mouseClock.getElapsedTime() > Seconds(1) ) {
		if ( uiWinMenu->getAlpha() == 255 && uiWinMenu->getActionManager()->isEmpty() ) {
			uiWinMenu->runAction( Actions::Fade::New( 255, 0, Milliseconds(250) ) );
		}
	}

	if ( updateLayout && waitClock.getElapsedTime().asMilliseconds() > 250.f ) {
		refreshLayout();
	}

	if ( updateStyleSheet && cssWaitClock.getElapsedTime().asMilliseconds() > 250.f ) {
		refreshStyleSheet();
	}

	SceneManager::instance()->update();

	if ( appUiSceneNode->invalidated() || uiSceneNode->invalidated() ) {
		window->clear();

		SceneManager::instance()->draw();

		window->display();
	} else {
		Sys::sleep( Milliseconds(8) );
	}
}

void imagePathOpen( const Event * event ) {
	UICommonDialog * CDL = reinterpret_cast<UICommonDialog*> ( event->getNode() );

	loadImagesFromFolder( CDL->getFullPath() );
}

void fontPathOpen( const Event * event ) {
	UICommonDialog * CDL = reinterpret_cast<UICommonDialog*> ( event->getNode() );

	loadFontsFromFolder( CDL->getFullPath() );
}

void styleSheetPathOpen( const Event * event ) {
	UICommonDialog * CDL = reinterpret_cast<UICommonDialog*> ( event->getNode() );

	loadStyleSheet( CDL->getFullPath() );
}

void layoutOpen( const Event * event ) {
	UICommonDialog * CDL = reinterpret_cast<UICommonDialog*> ( event->getNode() );

	loadLayout( CDL->getFullPath() );
}

void projectOpen( const Event * event ) {
	UICommonDialog * CDL = reinterpret_cast<UICommonDialog*> ( event->getNode() );

	loadProject( CDL->getFullPath() );
}

void fileMenuClick( const Event * event ) {
	if ( !event->getNode()->isType( UI_TYPE_MENUITEM ) )
		return;

	const String& txt = reinterpret_cast<UIMenuItem*> ( event->getNode() )->getText();

	UITheme * prevTheme = UIThemeManager::instance()->getDefaultTheme();
	UIThemeManager::instance()->setDefaultTheme( theme );

	if ( "Open project..." == txt ) {
		UICommonDialog * TGDialog = UICommonDialog::New( UI_CDL_DEFAULT_FLAGS, "*.xml" );
		TGDialog->setTheme( theme );
		TGDialog->setWinFlags( UI_WIN_DEFAULT_FLAGS | UI_WIN_MAXIMIZE_BUTTON | UI_WIN_MODAL );
		TGDialog->setTitle( "Open layout..." );
		TGDialog->addEventListener( Event::OpenFile, cb::Make1( projectOpen ) );
		TGDialog->center();
		TGDialog->show();
	} else if ( "Open layout..." == txt ) {
		UICommonDialog * TGDialog = UICommonDialog::New( UI_CDL_DEFAULT_FLAGS, "*.xml" );
		TGDialog->setTheme( theme );
		TGDialog->setWinFlags( UI_WIN_DEFAULT_FLAGS | UI_WIN_MAXIMIZE_BUTTON | UI_WIN_MODAL );
		TGDialog->setTitle( "Open layout..." );
		TGDialog->addEventListener( Event::OpenFile, cb::Make1( layoutOpen ) );
		TGDialog->center();
		TGDialog->show();
	} else if ( "Close" == txt ) {
		closeProject();
	} else if ( "Quit" == txt ) {
		onCloseRequestCallback( window );
	} else if ( "Load images from path..." == txt ) {
		UICommonDialog * TGDialog = UICommonDialog::New( UI_CDL_DEFAULT_FLAGS | CDL_FLAG_ALLOW_FOLDER_SELECT );
		TGDialog->setTheme( theme );
		TGDialog->setWinFlags( UI_WIN_DEFAULT_FLAGS | UI_WIN_MAXIMIZE_BUTTON | UI_WIN_MODAL );
		TGDialog->setTitle( "Open images from folder..." );
		TGDialog->addEventListener( Event::OpenFile, cb::Make1( imagePathOpen ) );
		TGDialog->center();
		TGDialog->show();
	} else if ( "Load fonts from path..." == txt ) {
		UICommonDialog * TGDialog = UICommonDialog::New( UI_CDL_DEFAULT_FLAGS | CDL_FLAG_ALLOW_FOLDER_SELECT );
		TGDialog->setTheme( theme );
		TGDialog->setWinFlags( UI_WIN_DEFAULT_FLAGS | UI_WIN_MAXIMIZE_BUTTON | UI_WIN_MODAL );
		TGDialog->setTitle( "Open fonts from folder..." );
		TGDialog->addEventListener( Event::OpenFile, cb::Make1( fontPathOpen ) );
		TGDialog->center();
		TGDialog->show();
	} else if ( "Load style sheet from path..." == txt ) {
		UICommonDialog * TGDialog = UICommonDialog::New( UI_CDL_DEFAULT_FLAGS, "*.css" );
		TGDialog->setTheme( theme );
		TGDialog->setWinFlags( UI_WIN_DEFAULT_FLAGS | UI_WIN_MAXIMIZE_BUTTON | UI_WIN_MODAL );
		TGDialog->setTitle( "Open style sheet from path..." );
		TGDialog->addEventListener( Event::OpenFile, cb::Make1( styleSheetPathOpen ) );
		TGDialog->center();
		TGDialog->show();
	}

	UIThemeManager::instance()->setDefaultTheme( prevTheme );
}

EE_MAIN_FUNC int main (int argc, char * argv []) {
	fileWatcher = new efsw::FileWatcher();
	listener = new UpdateListener();
	fileWatcher->watch();

	Display * currentDisplay = Engine::instance()->getDisplayManager()->getDisplayIndex(0);
	Float pixelDensity = PixelDensity::toFloat( currentDisplay->getPixelDensity() );
	DisplayMode currentMode = currentDisplay->getCurrentMode();

	Uint32 width = eemin( currentMode.Width, (Uint32)( 1280 * pixelDensity ) );
	Uint32 height = eemin( currentMode.Height, (Uint32)( 720 * pixelDensity ) );

	window = Engine::instance()->createWindow( WindowSettings( width, height, "eepp - UI Editor", WindowStyle::Default, WindowBackend::Default, 32, "assets/icon/ee.png", pixelDensity ), ContextSettings( true, GLv_default, true, 24, 1, 0, false ) );

	if ( window->isOpen() ) {
		window->setCloseRequestCallback( cb::Make1( onCloseRequestCallback ) );

		uiSceneNode = UISceneNode::New();
		SceneManager::instance()->add( uiSceneNode );

		appUiSceneNode = UISceneNode::New();
		SceneManager::instance()->add( appUiSceneNode );

		appUiSceneNode->enableDrawInvalidation();
		uiSceneNode->enableDrawInvalidation();

		std::string pd;
		if ( PixelDensity::getPixelDensity() >= 1.5f ) pd = "1.5x";
		else if ( PixelDensity::getPixelDensity() >= 2.f ) pd = "2x";

		FontTrueType * font = FontTrueType::New( "NotoSans-Regular", "assets/fonts/NotoSans-Regular.ttf" );

		theme = UITheme::load( "uitheme" + pd, "uitheme" + pd, "assets/ui/uitheme" + pd + ".eta", font, "assets/ui/uitheme.css" );

		appUiSceneNode->combineStyleSheet( theme->getStyleSheet() );

		UIThemeManager::instance()->setDefaultEffectsEnabled( true )->setDefaultTheme( theme )->setDefaultFont( font )->add( theme );

		loadConfig();

		SceneManager::instance()->setCurrentUISceneNode( appUiSceneNode );

		uiWinMenu = UIWinMenu::New();

		UIPopUpMenu * uiPopMenu = UIPopUpMenu::New();
		uiPopMenu->add( "Open project...", theme->getIconByName( "document-open" ) );
		uiPopMenu->addSeparator();
		uiPopMenu->add( "Open layout...", theme->getIconByName( "document-open" ) );
		uiPopMenu->addSeparator();
		uiPopMenu->addSubMenu( "Recent projects", NULL, UIPopUpMenu::New() );
		uiPopMenu->addSeparator();
		uiPopMenu->add( "Close", theme->getIconByName( "document-close" ) );
		uiPopMenu->addSeparator();
		uiPopMenu->add( "Quit", theme->getIconByName( "quit" ) );
		uiWinMenu->addMenuButton( "File", uiPopMenu );
		uiPopMenu->addEventListener( Event::OnItemClicked, cb::Make1( fileMenuClick ) );

		UIPopUpMenu * uiResourceMenu = UIPopUpMenu::New();
		uiResourceMenu->add( "Load images from path...", theme->getIconByName( "document-open" ) );
		uiResourceMenu->addSeparator();
		uiResourceMenu->add( "Load fonts from path...", theme->getIconByName( "document-open" ) );
		uiResourceMenu->addSeparator();
		uiResourceMenu->add( "Load style sheet from path...", theme->getIconByName( "document-open" ) );
		uiWinMenu->addMenuButton( "Resources", uiResourceMenu );
		uiResourceMenu->addEventListener( Event::OnItemClicked, cb::Make1( fileMenuClick ) );

		SceneManager::instance()->setCurrentUISceneNode( uiSceneNode );

		uiContainer = UIWidget::New();
		uiContainer->setId( "appContainer" )->setSize( uiSceneNode->getSize() );
		uiContainer->clipDisable();

		updateRecentProjects();

		resizeCb( window );

		window->pushResizeCallback( cb::Make1( resizeCb ) );

		if ( argc >= 2 ) {
			if ( argc >= 3 ) {
				loadStyleSheet( argv[2] );
			}

			loadLayout( argv[1] );
		}

		window->runMainLoop( &mainLoop );
	}

	saveConfig();

	Engine::destroySingleton();

	MemoryManager::showResults();

	delete fileWatcher;

	delete listener;

	return EXIT_SUCCESS;
}