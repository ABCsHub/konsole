#ifndef SESSIONCONTROLLER_H
#define SESSIONCONTROLLER_H

// Qt
#include <QIcon>
#include <QList>
#include <QPointer>
#include <QString>

// KDE
#include <KActionCollection>
#include <KXMLGUIClient>

// Konsole
#include "ViewProperties.h"
class QAction;
class TESession;
class TEWidget;
class UrlFilter;

// SaveHistoryTask
class TerminalCharacterDecoder;
class KJob;

namespace KIO
{
    class Job;
};

/**
 * Provides the actions associated with a session in the Konsole main menu
 * and exposes information such as the title and icon associated with the session to view containers.
 *
 * Each view should have one SessionController associated with it
 *
 * The SessionController will delete itself if either the view or the session is destroyed, for 
 * this reason it is recommended that other classes which need a pointer to a SessionController
 * use QPointer<SessionController> rather than SessionController* 
 */
class SessionController : public ViewProperties , public KXMLGUIClient
{
Q_OBJECT
    
public:
    /**
     * Constructs a new SessionController which operates on @p session and @p view.
     */
    SessionController(TESession* session , TEWidget* view, QObject* parent);
    ~SessionController();

    /** Reimplemented to watch for events happening to the view */
    virtual bool eventFilter(QObject* watched , QEvent* event);

    /** Returns the session associated with this controller */
    TESession* session() { return _session; }
    /** Returns the view associated with this controller */
    TEWidget*  view()    { return _view;    }

signals:
    /**
     * Emitted when the view associated with the controller is focused.  
     * This can be used by other classes to plug the controller's actions into a window's
     * menus. 
     */
    void focused( SessionController* controller );

private slots:
    // menu item handlers
    void copy();
    void paste();
    void clear();
    void clearAndReset();
    void searchHistory();
    void findNextInHistory();
    void findPreviousInHistory();
    void saveHistory();
    void clearHistory();
    void closeSession();
    void monitorActivity(bool monitor);
    void monitorSilence(bool monitor);

    void sessionStateChanged(TESession* session,int state);
    void sessionTitleChanged();

private:
    void setupActions();

private:
    TESession* _session;
    TEWidget*  _view;
    KIcon      _sessionIcon;
    QString    _sessionIconName;
    int        _previousState;
    
    UrlFilter* _viewUrlFilter;

    static KIcon _activityIcon;
    static KIcon _silenceIcon;

};

/** 
 * Abstract class representing a task which can be performed on a group of sessions.
 *
 * Create a new instance of the appropriate sub-class for the task you want to perform and
 * call the addSession() method to add each session which needs to be processed.
 *
 * Finally, call the execute() method to perform the sub-class specific action on each
 * of the sessions.
 */
class SessionTask : public QObject
{
Q_OBJECT

public:
   SessionTask();

   /** 
    * Sets whether the task automatically deletes itself when the task has been finished.
    * Depending on whether the task operates synchronously or asynchronously, the deletion
    * may be scheduled immediately after execute() returns or it may happen some time later.
    */
   void setAutoDelete(bool enable);
   /** Returns true if the task automatically deletes itself.  See setAutoDelete() */
   bool autoDelete() const;

   /** Adds a new session to the group */
   void addSession(TESession* session);

   /** 
    * Executes the task on each of the sessions in the group.
    * The completed() signal is emitted when the task is finished, depending on the specific sub-class
    * execute() may be synchronous or asynchronous
    */
   virtual void execute() = 0;

signals:
   /** 
    * Emitted when the task has completed.  
    * Depending on the task this may occur just before execute() returns, or it
    * may occur later
    */
   void completed();

protected:
   typedef QPointer<TESession> SessionPtr;

   /** Returns a list of sessions in the group */
   QList< SessionPtr > sessions() const;

private:

   bool _autoDelete;
   QList< SessionPtr > _sessions; 
};

/**
 * A task which prompts for a URL for each session and saves that session's output
 * to the given URL
 */
class SaveHistoryTask : public SessionTask
{
Q_OBJECT
  
public:
    /** Constructs a new task to save session output to URLs */
    SaveHistoryTask();
    virtual ~SaveHistoryTask();

    /**
     * Opens a save file dialog for each session in the group and begins saving
     * each session's history to the given URL.
     *
     * The data transfer is performed asynchronously and will continue after execute() returns.
     */
    virtual void execute();

private slots:
    void jobDataRequested(KIO::Job* job , QByteArray& data);
    void jobResult(KJob* job);

private:
    class SaveJob // structure to keep information needed to process
                  // incoming data requests from jobs
    {
    public:
        SessionPtr session; // the session associated with a history save job
        int lastLineFetched; // the last line processed in the previous data request
                             // set this to -1 at the start of the save job
        
        TerminalCharacterDecoder* decoder;  // decoder used to convert terminal characters
                                            // into output

    };

    QHash<KJob*,SaveJob> _jobSession;
};

/**
 * A task which searches through the output of sessions for matches for a given regular expression.
 * 
 * TODO - Implementation requirements:
 *          - The search is performed asynchronously in another thread when execute() is called.
 *          - Remember where the search got to when it reaches the end of the output in each session
 *            calling execute() subsequently should continue the search.
 *            This allows the class to be used for both the "Search history for text" 
 *            and new-in-KDE-4 "Monitor output for text" actions   
 *
 * TODO:  Implement this
 */
class SearchHistoryTask : public SessionTask
{
Q_OBJECT

public:
    SearchHistoryTask();

    /** Sets the regular expression which is searched for when execute() is called */
    void setRegExp(const QRegExp& regExp);
    /** Returns the regular expression which is searched for when execute() is called */
    QRegExp regExp() const;

    virtual void execute();

signals:
    /** 
     * Emitted when a match for the regular expression is found in a session's output.
     * The line numbers are given as offsets from the start of the history
     *
     * @param session The session in which a match for regExp() was found.
     * @param startLine The line in the output where the matched text starts
     * @param startColumn The column in the output where the matched text starts
     * @param endLine The line in the output where the matched text ends
     * @param endColumn The column in the output where the matched text ends 
     */
    void foundMatch(TESession* session , int startLine , int startColumn , int endLine , int endColumn );
    
private:
    QRegExp _regExp;
};

#endif //SESSIONCONTROLLER_H
