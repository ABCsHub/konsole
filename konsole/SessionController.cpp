
// KDE
#include <KAction>
#include <KIcon>
#include <KLocale>
#include <KRun>
#include <KToggleAction>
#include <KUrl>
#include <KXMLGUIFactory>
#include <kdebug.h>

// Konsole
#include "EditSessionDialog.h"
#include "Filter.h"
#include "HistorySizeDialog.h"
#include "IncrementalSearchBar.h"
#include "Session.h"
#include "TerminalDisplay.h"
#include "SessionController.h"
#include "ProcessInfo.h"

// for SaveHistoryTask
#include <KUrl>
#include <KFileDialog>
#include <KJob>
#include <KMessageBox>
#include "TerminalCharacterDecoder.h"

// used an old-style include below because #include <KIO/Job> does not work
// at the time of writing
#include <kio/job.h>

using namespace Konsole;

KIcon SessionController::_activityIcon;
KIcon SessionController::_silenceIcon;
QPointer<SearchHistoryThread> SearchHistoryTask::_thread;

SessionController::SessionController(Session* session , TerminalDisplay* view, QObject* parent)
    : ViewProperties(parent)
    , KXMLGUIClient()
    , _session(session)
    , _view(view)
    , _previousState(-1)
    , _viewUrlFilter(0)
    , _searchFilter(0)
    , _searchToggleAction(0)
{
    // handle user interface related to session (menus etc.)
    setXMLFile("konsole/sessionui.rc");
    setupActions();

    setIdentifier(_session->sessionId());
    sessionTitleChanged();
    
    view->installEventFilter(this);

    // listen for popup menu requests
    connect( _view , SIGNAL(configureRequest(TerminalDisplay*,int,int,int)) , this,
            SLOT(showDisplayContextMenu(TerminalDisplay*,int,int,int)) );

    // listen to activity / silence notifications from session
    connect( _session , SIGNAL(notifySessionState(Session*,int)) , this ,
            SLOT(sessionStateChanged(Session*,int) ));

    // listen to title and icon changes
    connect( _session , SIGNAL(updateTitle()) , this , SLOT(sessionTitleChanged()) );

    // install filter on the view to highlight URLs
    view->filterChain()->addFilter( new UrlFilter() );

    // take a snapshot of the session state every so often when
    // user activity occurs
    QTimer* activityTimer = new QTimer(this);
    activityTimer->setSingleShot(true);
    activityTimer->setInterval(2000);
    connect( _view , SIGNAL(keyPressedSignal(QKeyEvent*)) , activityTimer , SLOT(start()) );
    connect( activityTimer , SIGNAL(timeout()) , this , SLOT(snapshot()) );
}

SessionController::~SessionController()
{ 
}

void SessionController::snapshot()
{
    qDebug() << "session" << _session->title() << "snapshot";
}

KUrl SessionController::url() const
{
    ProcessInfo* info = ProcessInfo::newInstance(_session->sessionPid());
    info->update();

    QString path;
    if ( info->isValid() )
    {
        bool ok = false;

        // check if foreground process is bookmark-able
        int pid = info->foregroundPid(&ok);
        if ( ok )
        {
            qDebug() << "reading session process = " << info->name(&ok);

            ProcessInfo* foregroundInfo = ProcessInfo::newInstance(pid);
            foregroundInfo->update();

            qDebug() << "reading foreground process = " << foregroundInfo->name(&ok);

            // for remote connections, save the user and host
            // bright ideas to get the directory at the other end are welcome :)
            if ( foregroundInfo->name(&ok) == "ssh" && ok )
            {
                SSHProcessInfo sshInfo(*foregroundInfo);
                path = "ssh://" + sshInfo.userName() + '@' + sshInfo.host();
            }
            else
            {
                path = foregroundInfo->currentDir(&ok);

                if (!ok)
                    path = QString::null;
            }

            delete foregroundInfo;
        }
        else // otherwise use the current working directory of the shell process
        {
            path = info->currentDir(&ok); 
            if (!ok)
                path = QString::null;
        }
    }

    delete info;
    return KUrl( path ); 
}

void SessionController::openUrl( const KUrl& url ) 
{
    // handle local paths
    if ( url.isLocalFile() )
    {
        QString path = url.toLocalFile();
        KRun::shellQuote(path);

        _session->emulation()->sendText("cd " + path + '\r');
    }
    else if ( url.protocol() == "ssh" )
    {
        _session->emulation()->sendText("ssh ");

        if ( url.hasUser() )
            _session->emulation()->sendText(url.user() + '@');
        if ( url.hasHost() )
            _session->emulation()->sendText(url.host() + '\r');
    }
    else
    {
        //TODO Implement handling for other Url types
        qWarning() << "Unable to open bookmark at url" << url << ", I do not know"
           << " how to handle the protocol " << url.protocol(); 
    }
}

bool SessionController::eventFilter(QObject* watched , QEvent* event)
{
    if ( watched == _view )
    {
        if ( event->type() == QEvent::FocusIn )
        {
            // notify the world that the view associated with this session has been focused
            // used by the view manager to update the title of the MainWindow widget containing the view
            emit focused(this);

            // when the view is focused, set bell events from the associated session to be delivered
            // by the focused view
       
            // first, disconnect any other views which are listening for bell signals from the session 
            disconnect( _session , SIGNAL(bellRequest(const QString&)) , 0 , 0 );
            // second, connect the newly focused view to listen for the session's bell signal
            connect( _session , SIGNAL(bellRequest(const QString&)) ,
                    _view , SLOT(bell(const QString&)) );
        }
    }

    return false;
}

void SessionController::removeSearchFilter()
{
    if (!_searchFilter)
        return;

    _view->filterChain()->removeFilter(_searchFilter);
    delete _searchFilter;
    _searchFilter = 0;
}

void SessionController::setSearchBar(IncrementalSearchBar* searchBar) 
{
    // disconnect the existing search bar
    if ( _searchBar ) 
    {
        disconnect( this , 0 , _searchBar , 0 );
        disconnect( _searchBar , 0 , this , 0 );
    }

    // remove any existing search filter
    removeSearchFilter(); 

    // connect new search bar
    _searchBar = searchBar;
    if ( _searchBar )
    {
        connect( _searchBar , SIGNAL(closeClicked()) , this , SLOT(searchClosed()) );
        connect( _searchBar , SIGNAL(findNextClicked()) , this , SLOT(findNextInHistory()) );
        connect( _searchBar , SIGNAL(findPreviousClicked()) , this , SLOT(findPreviousInHistory()) ); 
  
        // if the search bar was previously active 
        // then re-enter search mode 
        searchHistory( _searchToggleAction->isChecked() ); 
    } 
}
IncrementalSearchBar* SessionController::searchBar() const
{
    return _searchBar;
}

void SessionController::setupActions()
{
    QAction* action = 0;
    KToggleAction* toggleAction = 0;
    KActionCollection* collection = actionCollection();

    // Save Session
    action = collection->addAction("save-session");
    action->setIcon( KIcon("save") );
    action->setText( i18n("&Save Session") );
    connect( action , SIGNAL(triggered()) , this , SLOT(saveSession()) );

    // Close Session
    action = collection->addAction("close-session"); 
    action->setIcon( KIcon("window-close") ); // FIXME: Not the best icon for this
    action->setText( i18n("&Close Tab") );
    action->setShortcut( QKeySequence(Qt::CTRL+Qt::SHIFT+Qt::Key_W) );
    connect( action , SIGNAL(triggered()) , this , SLOT(closeSession()) );
    
    // Copy and Paste
    action = collection->addAction("copy");
    action->setIcon( KIcon("edit-copy") );
    action->setText( i18n("&Copy") );
    action->setShortcut( QKeySequence(Qt::CTRL+Qt::SHIFT+Qt::Key_C) );
    connect( action , SIGNAL(triggered()) , this , SLOT(copy()) );
    
    action = collection->addAction("paste");
    action->setIcon( KIcon("edit-paste") );
    action->setText( i18n("&Paste") );
    action->setShortcut( QKeySequence(Qt::CTRL+Qt::SHIFT+Qt::Key_V) );
    connect( action , SIGNAL(triggered()) , this , SLOT(paste()) );

    // Send to All
    toggleAction = new KToggleAction(i18n("Send Input to All"),this);
    action = collection->addAction("send-input-to-all",toggleAction);
    connect( action , SIGNAL(triggered()) , this , SLOT(sendInputToAll()) );

    // Clear and Clear+Reset
    action = collection->addAction("clear");
    action->setText( i18n("C&lear Display") );
    connect( action , SIGNAL(triggered()) , this , SLOT(clear()) );
    
    action = collection->addAction("clear-and-reset");
    action->setText( i18n("Clear and Reset") );
    action->setIcon( KIcon("history-clear") );
    connect( action , SIGNAL(triggered()) , this , SLOT(clearAndReset()) );

    // Monitor
    toggleAction = new KToggleAction(i18n("Monitor for &Activity"),this);  
    toggleAction->setShortcut( QKeySequence(Qt::CTRL+Qt::SHIFT+Qt::Key_A) );
    action = collection->addAction("monitor-activity",toggleAction);
    connect( action , SIGNAL(toggled(bool)) , this , SLOT(monitorActivity(bool)) );

    toggleAction = new KToggleAction(i18n("Monitor for &Silence"),this);  
    toggleAction->setShortcut( QKeySequence(Qt::CTRL+Qt::SHIFT+Qt::Key_I) );
    action = collection->addAction("monitor-silence",toggleAction);
    connect( action , SIGNAL(toggled(bool)) , this , SLOT(monitorSilence(bool)) );

    // Text Size
    action = collection->addAction("increase-text-size");
    action->setText( i18n("Increase Text Size") );
    action->setIcon( KIcon("zoom-in") );
    action->setShortcut( QKeySequence(Qt::CTRL+Qt::Key_Plus) );
    connect( action , SIGNAL(triggered()) , this , SLOT(increaseTextSize()) );

    action = collection->addAction("decrease-text-size");
    action->setText( i18n("Decrease Text Size") );
    action->setIcon( KIcon("zoom-out") );
    action->setShortcut( QKeySequence(Qt::CTRL+Qt::SHIFT+Qt::Key_Minus) );
    connect( action , SIGNAL(triggered()) , this , SLOT(decreaseTextSize()) );

    // History
    _searchToggleAction = new KAction(i18n("Search History"),this);
    _searchToggleAction->setShortcut( QKeySequence(Qt::CTRL+Qt::SHIFT+Qt::Key_F) );
    _searchToggleAction->setIcon( KIcon("edit-find") );
    action = collection->addAction("search-history" , _searchToggleAction);
    connect( action , SIGNAL(triggered()) , this , SLOT(searchHistory()) );
    
    action = collection->addAction("find-next");
    action->setIcon( KIcon("find-next") );
    action->setText( i18n("Find Next") );
    action->setShortcut( QKeySequence(Qt::Key_F3) );
    connect( action , SIGNAL(triggered()) , this , SLOT(findNextInHistory()) );
    
    action = collection->addAction("find-previous");
    action->setIcon( KIcon("find-previous") );
    action->setText( i18n("Find Previous") );
    action->setShortcut( QKeySequence(Qt::SHIFT + Qt::Key_F3) );
    connect( action , SIGNAL(triggered()) , this , SLOT(findPreviousInHistory()) );
    
    action = collection->addAction("save-history");
    action->setText( i18n("Save History") );
    connect( action , SIGNAL(triggered()) , this , SLOT(saveHistory()) );
   
    action = collection->addAction("history-options");
    action->setText( i18n("History Options") );
    action->setIcon( KIcon("configure") );
    connect( action , SIGNAL(triggered()) , this , SLOT(historyOptions()) );

    action = collection->addAction("clear-history");
    action->setText( i18n("Clear History") );
    connect( action , SIGNAL(triggered()) , this , SLOT(clearHistory()) );

    action = collection->addAction("clear-history-and-reset");
    action->setText( i18n("Clear History && Reset") );
    action->setShortcut( QKeySequence(Qt::CTRL+Qt::SHIFT+Qt::Key_X) );
    connect( action , SIGNAL(triggered()) , this , SLOT(clearHistoryAndReset()) );

    // Terminal Options 
    action = collection->addAction("terminal-options");
    action->setText( i18n("Terminal Options...") );
    connect( action , SIGNAL(triggered()) , this , SLOT(showTerminalOptions()) );

    // debugging tools
    //action = collection->addAction("debug-process");
    //action->setText( "Get Foreground Process" );
    //connect( action , SIGNAL(triggered()) , this , SLOT(debugProcess()) );
}

void SessionController::debugProcess()
{
    // testing facility to retrieve process information about 
    // currently active process in the shell
    ProcessInfo* sessionProcess = ProcessInfo::newInstance(_session->sessionPid());
    sessionProcess->update();

    bool ok = false;
    int fpid = sessionProcess->foregroundPid(&ok);

    if ( ok )
    {
        ProcessInfo* fp = ProcessInfo::newInstance(fpid);
        fp->update();

        QString name = fp->name(&ok);

        if ( ok )
        {
            _session->setTitle(name);
            sessionTitleChanged();
        }

        QString currentDir = fp->currentDir(&ok);

        if ( ok )
        {
            qDebug() << currentDir;
        }
        else
        {
            qDebug() << "could not read current dir of foreground process";
        }

        delete fp;
    }
    delete sessionProcess;
}

void SessionController::showTerminalOptions()
{
    EditSessionDialog dialog(_view);
    dialog.setSessionType(_session->type());
    int result = dialog.exec();
}
void SessionController::saveSession()
{
    //SaveSessionDialog dialog(_view);
    //int result = dialog.exec();
}
void SessionController::closeSession()
{
    _session->closeSession();
}

void SessionController::copy()
{
    _view->copyClipboard(); 
}

void SessionController::paste()
{
    _view->pasteClipboard();
}

void SessionController::clear()
{
    Emulation* emulation = _session->emulation();

    emulation->clearEntireScreen();
}
void SessionController::clearAndReset()
{
    Emulation* emulation = _session->emulation();

    emulation->reset();
}
void SessionController::searchClosed()
{
    searchHistory(false); 
}

void SessionController::searchHistory()
{
    searchHistory(true);
}

// searchHistory() may be called either as a result of clicking a menu item or 
// as a result of changing the search bar widget
void SessionController::searchHistory(bool showSearchBar)
{
    if ( _searchBar )
    {
        _searchBar->setVisible(showSearchBar);

        if (showSearchBar)
        {
            removeSearchFilter();

            _searchFilter = new RegExpFilter();
            _view->filterChain()->addFilter(_searchFilter);
            connect( _searchBar , SIGNAL(searchChanged(const QString&)) , this , 
                    SLOT(searchTextChanged(const QString&)) );         
   
            // invoke search for matches for the current search text 
            const QString& currentSearchText = _searchBar->searchText();
            if (!currentSearchText.isEmpty())
            {
                searchTextChanged(currentSearchText);
            }

            //SessionTask* task = new SearchHistoryTask();
            //task->setAutoDelete(true);
            //task->addSession( _session );
            //task->execute();
        }
        else
        {
            disconnect( _searchBar , SIGNAL(searchChanged(const QString&)) , this ,
                    SLOT(searchTextChanged(const QString&)) );

            removeSearchFilter();
            
            _view->setFocus( Qt::ActiveWindowFocusReason );
        }
    }
}
void SessionController::searchTextChanged(const QString& text)
{
    Q_ASSERT( _searchBar );

    Qt::CaseSensitivity caseHandling = _searchBar->matchCase() ? Qt::CaseSensitive : Qt::CaseInsensitive;
    QRegExp::PatternSyntax syntax = _searchBar->matchRegExp() ? QRegExp::RegExp : QRegExp::FixedString;
    
    QRegExp regExp( text.trimmed() ,  caseHandling , syntax );

    if ( !regExp.isEmpty() )
    {
        SearchHistoryTask* task = new SearchHistoryTask(this); 
       
        task->setRegExp(regExp);
        task->setMatchCase( _searchBar->matchCase() );
        task->setMatchRegExp( _searchBar->matchRegExp() );
        task->setSearchDirection( SearchHistoryTask::Forwards ); 
        task->setAutoDelete(true);
        task->addSession( _session );   
        task->execute();
    }

    _searchFilter->setRegExp(regExp);

    _view->processFilters();

    // color search bar to indicate whether a match was found    
    if ( _searchFilter->hotSpots().count() > 0 )
    {
        _searchBar->setFoundMatch(true);
    } 
    else 
    {
        _searchBar->setFoundMatch(false);
    }    
    // TODO - Optimise by only updating affected regions
    _view->update();
}
void SessionController::findNextInHistory()
{
    qDebug() << "find next";
}
void SessionController::findPreviousInHistory()
{
    qDebug() << "find previous";
}
void SessionController::historyOptions()
{
    HistorySizeDialog* dialog = new HistorySizeDialog(_view);
    const HistoryType& currentHistory = _session->history();

    if ( currentHistory.isEnabled() )
    {
        if ( currentHistory.isUnlimited() )
            dialog->setMode( HistorySizeDialog::UnlimitedHistory );
        else
        {
            dialog->setMode( HistorySizeDialog::FixedSizeHistory );
            dialog->setLineCount( currentHistory.maximumLineCount() );
        }
    }
    else
        dialog->setMode( HistorySizeDialog::NoHistory );

    if ( dialog->exec() == QDialog::Accepted )
    {
        if ( dialog->mode() == HistorySizeDialog::NoHistory )
            _session->setHistory( HistoryTypeNone() );
        else if ( dialog->mode() == HistorySizeDialog::FixedSizeHistory )
            _session->setHistory( HistoryTypeBuffer(dialog->lineCount()) );
        else if ( dialog->mode() == HistorySizeDialog::UnlimitedHistory )
            _session->setHistory( HistoryTypeFile() );       
    }

    delete dialog;
}
void SessionController::saveHistory()
{
    SessionTask* task = new SaveHistoryTask();
    task->setAutoDelete(true);
    task->addSession( _session );   
    task->execute();
}
void SessionController::clearHistory()
{
    _session->clearHistory();
}
void SessionController::clearHistoryAndReset()
{
    clearAndReset();
    clearHistory();
}
void SessionController::increaseTextSize()
{
    QFont font = _view->getVTFont();
    font.setPointSize(font.pointSize()+1);
    _view->setVTFont(font);

    //TODO - Save this setting as a session default
}
void SessionController::decreaseTextSize()
{
    const int MinimumFontSize = 6;

    QFont font = _view->getVTFont();
    font.setPointSize( qMax(font.pointSize()-1,MinimumFontSize) );
    _view->setVTFont(font);

    //TODO - Save this setting as a session default
}
void SessionController::sendInputToAll()
{
    Q_ASSERT(0); // Not implemented yet
}
void SessionController::monitorActivity(bool monitor)
{
    _session->setMonitorActivity(monitor);
}
void SessionController::monitorSilence(bool monitor)
{
    _session->setMonitorSilence(monitor); 
}
void SessionController::sessionTitleChanged()
{
        if ( _sessionIconName != _session->iconName() )
        { 
            _sessionIconName = _session->iconName();
            _sessionIcon = KIcon( _sessionIconName );
            setIcon( _sessionIcon );
        }
         
       //TODO - use _session->displayTitle() here. 
       setTitle( _session->title() ); 
}

void SessionController::showDisplayContextMenu(TerminalDisplay* /*display*/ , int state, int x, int y)
{
    if ( factory() )
    {
        QMenu* popup = dynamic_cast<QMenu*>(factory()->container("session-popup-menu",this));
    
        Q_ASSERT( popup );

        popup->exec( _view->mapToGlobal(QPoint(x,y)) );
    }
    else
    {
        qWarning() << "Unable to display popup menu for session" 
                   << _session->title() 
                   << ", no GUI factory available to build the popup.";
    }
}

void SessionController::sessionStateChanged(Session* /*session*/ , int state)
{
    //TODO - Share icons across sessions ( possible using a static QHash<QString,QIcon> variable 
    // to create a cache of icons mapped from icon names? )

    if ( state == _previousState )
        return;

    _previousState = state;

    if ( state == NOTIFYACTIVITY )
    {
        if (_activityIcon.isNull())
        {
            _activityIcon = KIcon("activity");
        }

        setIcon(_activityIcon);
    } 
    else if ( state == NOTIFYSILENCE )
    {
        if (_silenceIcon.isNull())
        {
            _silenceIcon = KIcon("silence");
        }

        setIcon(_silenceIcon);
    }
    else if ( state == NOTIFYNORMAL )
    {
        if ( _sessionIconName != _session->iconName() )
        { 
            _sessionIconName = _session->iconName();
            _sessionIcon = KIcon( _sessionIconName );
        }
            
        setIcon( _sessionIcon );
    }
}

SessionTask::SessionTask(QObject* parent)
    :  QObject(parent)
    ,  _autoDelete(false)
{
}
void SessionTask::setAutoDelete(bool enable)
{
    _autoDelete = enable;
}
bool SessionTask::autoDelete() const
{
    return _autoDelete;
}
void SessionTask::addSession(Session* session)
{
    _sessions << session;
}
QList<SessionPtr> SessionTask::sessions() const
{
    return _sessions;
}

SaveHistoryTask::SaveHistoryTask(QObject* parent)
    : SessionTask(parent)
{
}
SaveHistoryTask::~SaveHistoryTask()
{
}

void SaveHistoryTask::execute()
{
    QListIterator<SessionPtr> iter(sessions());

    // TODO - prompt the user if the file already exists, currently existing files
    //        are always overwritten

    // TODO - think about the UI when saving multiple history sessions, if there are more than two or
    //        three then providing a URL for each one will be tedious

    // TODO - show a warning ( preferably passive ) if saving the history output fails
    //
    
     KFileDialog* dialog = new KFileDialog( QString(":konsole") /* check this */, 
                                               QString() , 0 /* no parent widget */);
     QStringList mimeTypes;
     mimeTypes << "text/plain";
     mimeTypes << "text/html";
     dialog->setMimeFilter(mimeTypes,"text/plain");

     // iterate over each session in the task and display a dialog to allow the user to choose where
     // to save that session's history.
     // then start a KIO job to transfer the data from the history to the chosen URL
    while ( iter.hasNext() )
    {
        SessionPtr session = iter.next();

        dialog->setCaption( i18n("Save Output from %1",session->title()) );
            
        int result = dialog->exec();

        if ( result != QDialog::Accepted )
            continue;

        KUrl url = dialog->selectedUrl(); 

        if ( !url.isValid() )
        { // UI:  Can we make this friendlier?
            KMessageBox::sorry( 0 , i18n("%1 is an invalid URL, the output could not be saved.") );
            continue;
        }

        KIO::TransferJob* job = KIO::put( url, 
                                          -1,   // no special permissions
                                          true, // overwrite existing files
                                          false,// do not resume an existing transfer
                                          !url.isLocalFile() // show progress information only for remote
                                                             // URLs
                                                             //
                                                             // a better solution would be to show progress
                                                             // information after a certain period of time
                                                             // instead, since the overall speed of transfer
                                                             // depends on factors other than just the protocol
                                                             // used
                                        );


        SaveJob jobInfo;
        jobInfo.session = session;
        jobInfo.lastLineFetched = -1;  // when each request for data comes in from the KIO subsystem
                                       // lastLineFetched is used to keep track of how much of the history
                                       // has already been sent, and where the next request should continue
                                       // from.  
                                       // this is set to -1 to indicate the job has just been started

        if ( dialog->currentMimeFilter() == "text/html" )
           jobInfo.decoder = new HTMLDecoder();
        else
           jobInfo.decoder = new PlainTextDecoder();

        _jobSession.insert(job,jobInfo);

        connect( job , SIGNAL(dataReq(KIO::Job*,QByteArray&)),
                 this, SLOT(jobDataRequested(KIO::Job*,QByteArray&)) );
        connect( job , SIGNAL(result(KJob*)),
                 this, SLOT(jobResult(KJob*)) );
    }

    dialog->deleteLater();   
}
void SaveHistoryTask::jobDataRequested(KIO::Job* job , QByteArray& data)
{
    // TODO - Report progress information for the job

    // PERFORMANCE:  Do some tests and tweak this value to get faster saving
    const int LINES_PER_REQUEST = 500;

    SaveJob& info = _jobSession[job];   

    // transfer LINES_PER_REQUEST lines from the session's history 
    // to the save location
    if ( info.session )
    {
        // note:  when retrieving lines from the emulation, 
        // the first line is at index 0.
        
        int sessionLines = info.session->emulation()->lines();

        if ( sessionLines-1 == info.lastLineFetched )
            return; // if there is no more data to transfer then stop the job

        int copyUpToLine = qMin( info.lastLineFetched + LINES_PER_REQUEST , 
                                 sessionLines-1 );

        QTextStream stream(&data,QIODevice::ReadWrite);
        info.session->emulation()->writeToStream( &stream , info.decoder , info.lastLineFetched+1 , copyUpToLine );

        // if there are still more lines to process after this request 
        // then insert a new line character
        // to ensure that the next block of lines begins on a new line       
        //
        // FIXME - There is still an extra new-line at the end of the save data.   
        if ( copyUpToLine <= sessionLines-1 )
        {
            stream << '\n';
        }


        info.lastLineFetched = copyUpToLine;
    }
}
void SaveHistoryTask::jobResult(KJob* job)
{
    if ( job->error() )
    {
        KMessageBox::sorry( 0 , i18n("A problem occurred when saving the output.\n%1",job->errorString()) );
    }

    SaveJob& info = _jobSession[job];

    _jobSession.remove(job);
    
    delete info.decoder;
  
    // notify the world that the task is done 
    emit completed();

    if ( autoDelete() )
        deleteLater();
}

void SearchHistoryTask::execute()
{
    Q_ASSERT( sessions().first() );

    Emulation* emulation = sessions().first()->emulation();

    if ( !_regExp.isEmpty() )
    {
        emulation->findTextBegin();
        emulation->findTextNext(_regExp.pattern() , true , false , false );
    }
}

SearchHistoryTask::SearchHistoryTask(QObject* parent)
    : SessionTask(parent)
    , _matchRegExp(false)
    , _matchCase(false)
    , _direction(Forwards)
{

}

void SearchHistoryTask::setMatchCase( bool matchCase )
{
    _matchCase = matchCase;
}
bool SearchHistoryTask::matchCase() const
{
    return _matchCase;
}
void SearchHistoryTask::setMatchRegExp( bool matchRegExp )
{
    _matchRegExp = matchRegExp;
}
bool SearchHistoryTask::matchRegExp() const
{
    return _matchRegExp;
}
void SearchHistoryTask::setSearchDirection( SearchDirection direction )
{
    _direction = direction;
}
SearchHistoryTask::SearchDirection SearchHistoryTask::searchDirection() const
{
    return _direction;
}
void SearchHistoryTask::setRegExp(const QRegExp& expression)
{
    _regExp = expression;
}
QRegExp SearchHistoryTask::regExp() const
{
    return _regExp;
}

#include "SessionController.moc"
