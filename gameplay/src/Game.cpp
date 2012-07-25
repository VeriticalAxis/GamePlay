#include "Base.h"
#include "Game.h"
#include "Platform.h"
#include "RenderState.h"
#include "FileSystem.h"
#include "FrameBuffer.h"
#include "SceneLoader.h"

/** @script{ignore} */
GLenum __gl_error_code = GL_NO_ERROR;
/** @script{ignore} */
ALenum __al_error_code = AL_NO_ERROR;

namespace gameplay
{

static Game* __gameInstance = NULL;
double Game::_pausedTimeLast = 0.0;
double Game::_pausedTimeTotal = 0.0;

Game::Game() 
    : _initialized(false), _state(UNINITIALIZED), 
      _frameLastFPS(0), _frameCount(0), _frameRate(0), 
      _clearDepth(1.0f), _clearStencil(0), _properties(NULL),
      _animationController(NULL), _audioController(NULL), 
      _physicsController(NULL), _aiController(NULL), _audioListener(NULL), 
      _gamepads(NULL), _timeEvents(NULL), _scriptController(NULL), _scriptListeners(NULL)
{
    GP_ASSERT(__gameInstance == NULL);
    __gameInstance = this;
    _gamepads = new std::vector<Gamepad*>;
    _timeEvents = new std::priority_queue<TimeEvent, std::vector<TimeEvent>, std::less<TimeEvent> >();
}

Game::~Game()
{
    if (_scriptListeners)
    {
        for (unsigned int i = 0; i < _scriptListeners->size(); i++)
        {
            SAFE_DELETE((*_scriptListeners)[i]);
        }
        SAFE_DELETE(_scriptListeners);
    }

    if (_scriptController)
    {
        _scriptController->finalize();
        SAFE_DELETE(_scriptController);
    }

    // Do not call any virtual functions from the destructor.
    // Finalization is done from outside this class.
    SAFE_DELETE(_timeEvents);
#ifdef GAMEPLAY_MEM_LEAK_DETECTION
    Ref::printLeaks();
    printMemoryLeaks();
#endif
}

Game* Game::getInstance()
{
    GP_ASSERT(__gameInstance);
    return __gameInstance;
}

double Game::getAbsoluteTime()
{
    return Platform::getAbsoluteTime();
}

double Game::getGameTime()
{
    return Platform::getAbsoluteTime() - _pausedTimeTotal;
}

void Game::setVsync(bool enable)
{
    Platform::setVsync(enable);
}

bool Game::isVsync()
{
    return Platform::isVsync();
}

int Game::run()
{
    if (_state != UNINITIALIZED)
        return -1;

    loadConfig();

    _width = Platform::getDisplayWidth();
    _height = Platform::getDisplayHeight();

    // Start up game systems.
    if (!startup())
    {
        shutdown();
        return -2;
    }
    return 0;
}

bool Game::startup()
{
    if (_state != UNINITIALIZED)
        return false;

    setViewport(Rectangle(0.0f, 0.0f, (float)_width, (float)_height));
    RenderState::initialize();
    FrameBuffer::initialize();
    
    _animationController = new AnimationController();
    _animationController->initialize();

    _audioController = new AudioController();
    _audioController->initialize();

    _physicsController = new PhysicsController();
    _physicsController->initialize();

    _aiController = new AIController();
    _aiController->initialize();

    loadGamepads();
    
    _scriptController = new ScriptController();
    _scriptController->initialize();

    // Set the script callback functions.
    if (_properties)
    {
        Properties* scripts = _properties->getNamespace("scripts", true);
        if (scripts)
        {
            const char* name;
            while ((name = scripts->getNextProperty()) != NULL)
            {
                ScriptController::ScriptCallback callback = ScriptController::toCallback(name);
                if (callback != ScriptController::INVALID_CALLBACK)
                {
                    std::string url = scripts->getString();
                    std::string file;
                    std::string id;
                    splitURL(url, &file, &id);

                    if (file.size() <= 0 || id.size() <= 0)
                    {
                        GP_ERROR("Invalid %s script callback function '%s'.", name, url.c_str());
                    }
                    else
                    {
                        _scriptController->loadScript(file.c_str());
                        _scriptController->registerCallback(callback, id);
                    }
                }
                else
                {
                    // Ignore everything else.
                }
            }
        }
    }

    _state = RUNNING;

    return true;
}

void Game::shutdown()
{
    // Call user finalization.
    if (_state != UNINITIALIZED)
    {
        GP_ASSERT(_animationController);
        GP_ASSERT(_audioController);
        GP_ASSERT(_physicsController);
        GP_ASSERT(_aiController);

        Platform::signalShutdown();
        finalize();

        
        std::vector<Gamepad*>::iterator itr = _gamepads->begin();
        std::vector<Gamepad*>::iterator end = _gamepads->end();
        while (itr != end)
        {
            SAFE_DELETE(*itr);
            itr++;
        }
        _gamepads->clear();
        SAFE_DELETE(_gamepads);
        
        _scriptController->finalizeGame();

        _animationController->finalize();
        SAFE_DELETE(_animationController);

        _audioController->finalize();
        SAFE_DELETE(_audioController);

        _physicsController->finalize();
        SAFE_DELETE(_physicsController);
        _aiController->finalize();
        SAFE_DELETE(_aiController);

        // Note: we do not clean up the script controller here
        // because users can call Game::exit() from a script.

        SAFE_DELETE(_audioListener);

        RenderState::finalize();

        SAFE_DELETE(_properties);

        _state = UNINITIALIZED;
    }
}

void Game::pause()
{
    if (_state == RUNNING)
    {
        GP_ASSERT(_animationController);
        GP_ASSERT(_audioController);
        GP_ASSERT(_physicsController);
        GP_ASSERT(_aiController);
        _state = PAUSED;
        _pausedTimeLast = Platform::getAbsoluteTime();
        _animationController->pause();
        _audioController->pause();
        _physicsController->pause();
        _aiController->pause();
    }
}

void Game::resume()
{
    if (_state == PAUSED)
    {
        GP_ASSERT(_animationController);
        GP_ASSERT(_audioController);
        GP_ASSERT(_physicsController);
        GP_ASSERT(_aiController);
        _state = RUNNING;
        _pausedTimeTotal += Platform::getAbsoluteTime() - _pausedTimeLast;
        _animationController->resume();
        _audioController->resume();
        _physicsController->resume();
        _aiController->resume();
    }
}

void Game::exit()
{
    shutdown();
}

void Game::frame()
{
    if (!_initialized)
    {
        initialize();
        _scriptController->initializeGame();
        _initialized = true;
    }

    if (_state == Game::RUNNING)
    {
        GP_ASSERT(_animationController);
        GP_ASSERT(_audioController);
        GP_ASSERT(_physicsController);
        GP_ASSERT(_aiController);

        // Update Time.
        static double lastFrameTime = Game::getGameTime();
        double frameTime = getGameTime();
        float elapsedTime = (frameTime - lastFrameTime);
        lastFrameTime = frameTime;

        // Update the scheduled and running animations.
        _animationController->update(elapsedTime);

        // Fire time events to scheduled TimeListeners
        fireTimeEvents(frameTime);
    
        // Update the physics.
        _physicsController->update(elapsedTime);

        // Update AI.
        _aiController->update(elapsedTime);

        // Application Update.
        update(elapsedTime);

        // Run script update.
        _scriptController->update(elapsedTime);

        // Audio Rendering.
        _audioController->update(elapsedTime);

        // Graphics Rendering.
        render(elapsedTime);

        // Run script render.
        _scriptController->render(elapsedTime);

        // Update FPS.
        ++_frameCount;
        if ((Game::getGameTime() - _frameLastFPS) >= 1000)
        {
            _frameRate = _frameCount;
            _frameCount = 0;
            _frameLastFPS = getGameTime();
        }
    }
    else
    {
        // Application Update.
        update(0);

        // Script update.
        _scriptController->update(0);

        // Graphics Rendering.
        render(0);

        // Script render.
        _scriptController->render(0);
    }
}

void Game::renderOnce(const char* function)
{
    _scriptController->executeFunction<void>(function, NULL);
    Platform::swapBuffers();
}

void Game::updateOnce()
{
    GP_ASSERT(_animationController);
    GP_ASSERT(_audioController);
    GP_ASSERT(_physicsController);
    GP_ASSERT(_aiController);

    // Update Time.
    static double lastFrameTime = getGameTime();
    double frameTime = getGameTime();
    float elapsedTime = (frameTime - lastFrameTime);
    lastFrameTime = frameTime;

    // Update the internal controllers.
    _animationController->update(elapsedTime);
    _physicsController->update(elapsedTime);
    _aiController->update(elapsedTime);
    _audioController->update(elapsedTime);
    _scriptController->update(elapsedTime);
}

void Game::setViewport(const Rectangle& viewport)
{
    _viewport = viewport;
    glViewport((GLuint)viewport.x, (GLuint)viewport.y, (GLuint)viewport.width, (GLuint)viewport.height); 
}

void Game::clear(ClearFlags flags, const Vector4& clearColor, float clearDepth, int clearStencil)
{
    GLbitfield bits = 0;
    if (flags & CLEAR_COLOR)
    {
        if (clearColor.x != _clearColor.x ||
            clearColor.y != _clearColor.y ||
            clearColor.z != _clearColor.z ||
            clearColor.w != _clearColor.w )
        {
            glClearColor(clearColor.x, clearColor.y, clearColor.z, clearColor.w);
            _clearColor.set(clearColor);
        }
        bits |= GL_COLOR_BUFFER_BIT;
    }

    if (flags & CLEAR_DEPTH)
    {
        if (clearDepth != _clearDepth)
        {
            glClearDepth(clearDepth);
            _clearDepth = clearDepth;
        }
        bits |= GL_DEPTH_BUFFER_BIT;

        // We need to explicitly call the static enableDepthWrite() method on StateBlock
        // to ensure depth writing is enabled before clearing the depth buffer (and to 
        // update the global StateBlock render state to reflect this).
        RenderState::StateBlock::enableDepthWrite();
    }

    if (flags & CLEAR_STENCIL)
    {
        if (clearStencil != _clearStencil)
        {
            glClearStencil(clearStencil);
            _clearStencil = clearStencil;
        }
        bits |= GL_STENCIL_BUFFER_BIT;
    }
    glClear(bits);
}

AudioListener* Game::getAudioListener()
{
    if (_audioListener == NULL)
    {
        _audioListener = new AudioListener();
    }
    return _audioListener;
}

void Game::menuEvent()
{
}

void Game::keyEvent(Keyboard::KeyEvent evt, int key)
{
}

void Game::touchEvent(Touch::TouchEvent evt, int x, int y, unsigned int contactIndex)
{
}

bool Game::mouseEvent(Mouse::MouseEvent evt, int x, int y, int wheelDelta)
{
    return false;
}

void Game::gamepadEvent(Gamepad::GamepadEvent evt, Gamepad* gamepad)
{
}

void Game::schedule(float timeOffset, TimeListener* timeListener, void* cookie)
{
    GP_ASSERT(_timeEvents);
    TimeEvent timeEvent(getGameTime() + timeOffset, timeListener, cookie);
    _timeEvents->push(timeEvent);
}

void Game::schedule(float timeOffset, const char* function)
{
    if (!_scriptListeners)
        _scriptListeners = new std::vector<ScriptListener*>();

    ScriptListener* listener = new ScriptListener(function);
    _scriptListeners->push_back(listener);
    schedule(timeOffset, listener, NULL);
}

void Game::fireTimeEvents(double frameTime)
{
    while (_timeEvents->size() > 0)
    {
        const TimeEvent* timeEvent = &_timeEvents->top();
        if (timeEvent->time > frameTime)
        {
            break;
        }
        if (timeEvent->listener)
        {
            timeEvent->listener->timeEvent(frameTime - timeEvent->time, timeEvent->cookie);
        }
        _timeEvents->pop();
    }
}

Game::ScriptListener::ScriptListener(const char* url)
{
    function = Game::getInstance()->getScriptController()->loadUrl(url);
}

void Game::ScriptListener::timeEvent(long timeDiff, void* cookie)
{
    Game::getInstance()->getScriptController()->executeFunction<void>(function.c_str(), "l", timeDiff);
}

Game::TimeEvent::TimeEvent(double time, TimeListener* timeListener, void* cookie)
    : time(time), listener(timeListener), cookie(cookie)
{
}

bool Game::TimeEvent::operator<(const TimeEvent& v) const
{
    // The first element of std::priority_queue is the greatest.
    return time > v.time;
}

Properties* Game::getConfig() const
{
    if (_properties == NULL)
        const_cast<Game*>(this)->loadConfig();

    return _properties;
}

void Game::loadConfig()
{
    if (_properties == NULL)
    {
        // Try to load custom config from file.
        if (FileSystem::fileExists("game.config"))
        {
            _properties = Properties::create("game.config");
            
            // Load filesystem aliases.
            Properties* aliases = _properties->getNamespace("aliases", true);
            if (aliases)
            {
                FileSystem::loadResourceAliases(aliases);
            }
        }
    }
}

void Game::loadGamepads()
{
    if (_properties)
    {
        // Check if there is a virtual keyboard included in the .config file.
        // If there is, try to create it and assign it to "player one".
        Properties* gamepadProperties = _properties->getNamespace("gamepads", true);
        if (gamepadProperties && gamepadProperties->exists("form"))
        {
            const char* gamepadFormPath = gamepadProperties->getString("form");
            GP_ASSERT(gamepadFormPath);
            Gamepad* gamepad = createGamepad(gamepadProperties->getId(), gamepadFormPath);
            GP_ASSERT(gamepad);
        }
    }
}

Gamepad* Game::createGamepad(const char* gamepadId, const char* gamepadFormPath)
{
    GP_ASSERT(gamepadFormPath);
    Gamepad* gamepad = new Gamepad(gamepadId, gamepadFormPath);
    GP_ASSERT(gamepad);
    _gamepads->push_back(gamepad);

    return gamepad;
}

}
