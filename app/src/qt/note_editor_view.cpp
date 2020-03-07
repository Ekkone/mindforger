/*
 note_editor_view.cpp     MindForger thinking notebook

 Copyright (C) 2016-2020 Martin Dvorak <martin.dvorak@mindforger.com>

 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU General Public License
 as published by the Free Software Foundation; either version 2
 of the License, or (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program. If not, see <http://www.gnu.org/licenses/>.
*/
#include "note_editor_view.h"

namespace m8r {

using namespace std;

inline bool caseInsensitiveLessThan(const QString &a, const QString &b)
{
    return a.compare(b, Qt::CaseInsensitive) < 0;
}

NoteEditorView::NoteEditorView(QWidget* parent)
    : QPlainTextEdit(parent),
      parent(parent),
      completedAndSelected(false)
{
    hitCounter = 0;

    setEditorFont(Configuration::getInstance().getEditorFont());
    setEditorTabWidth(Configuration::getInstance().getUiEditorTabWidth());

    // widgets
    highlighter = new NoteEditHighlight{document()};
    enableSyntaxHighlighting = Configuration::getInstance().isUiEditorEnableSyntaxHighlighting();
    tabsAsSpaces = Configuration::getInstance().isUiEditorTabsAsSpaces();
    tabWidth = Configuration::getInstance().getUiEditorTabWidth();
    highlighter->setEnabled(enableSyntaxHighlighting);
    // line numbers
    lineNumberPanel = new LineNumberPanel{this};
    lineNumberPanel->setVisible(showLineNumbers);
    // autocomplete
    model = new QStringListModel{this};
    completer = new QCompleter{this};
    completer->setWidget(this);
    // must be in unfiltered mode to show links
    completer->setCompletionMode(QCompleter::UnfilteredPopupCompletion);
    completer->setModel(model);
    completer->setModelSorting(QCompleter::CaseInsensitivelySortedModel);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setWrapAround(true);

    // signals
    QObject::connect(this, SIGNAL(blockCountChanged(int)), this, SLOT(updateLineNumberPanelWidth(int)));
    QObject::connect(this, SIGNAL(updateRequest(QRect,int)), this, SLOT(updateLineNumberPanel(QRect,int)));
    QObject::connect(this, SIGNAL(cursorPositionChanged()), this, SLOT(highlightCurrentLine()));
    QObject::connect(completer, SIGNAL(activated(const QString&)), this, SLOT(insertCompletion(const QString&)));
    // shortcut signals
    new QShortcut(
        QKeySequence(QKeySequence(Qt::CTRL+Qt::Key_Slash)),
        this, SLOT(slotStartLinkCompletion()));

    // capabilities
    setAcceptDrops(true);

    // show
    highlightCurrentLine();
    updateLineNumberPanelWidth(0);
}

/*
 * Configuration
 */

void NoteEditorView::setShowLineNumbers(bool show)
{
    showLineNumbers = show;
    lineNumberPanel->setVisible(showLineNumbers);
}

void NoteEditorView::setEditorTabWidth(int tabWidth)
{
    // tab width: 4 or 8
    QFontMetrics metrics(f);
    this->tabWidth = tabWidth;
    setTabStopWidth(tabWidth * metrics.width(' '));
}

void NoteEditorView::setEditorTabsAsSpacesPolicy(bool tabsAsSpaces)
{
    this->tabsAsSpaces = tabsAsSpaces;
}

void NoteEditorView::setEditorFont(std::string fontName)
{
    QFont editorFont;
    QString qFontName = QString::fromStdString(fontName);
    if(QString::compare(qFontName, "")==0) { // No font defined, set to default
        editorFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        Configuration::getInstance().setEditorFont(editorFont.toString().toUtf8().constData());
    } else {
        editorFont.fromString(qFontName);
        setFont(editorFont);
    }

}

void NoteEditorView::slotConfigurationUpdated()
{
    enableSyntaxHighlighting = Configuration::getInstance().isUiEditorEnableSyntaxHighlighting();
    highlighter->setEnabled(enableSyntaxHighlighting);

    setEditorTabWidth(Configuration::getInstance().getUiEditorTabWidth());
    setEditorTabsAsSpacesPolicy(Configuration::getInstance().isUiEditorTabsAsSpaces());
    setEditorFont(Configuration::getInstance().getEditorFont());
}

/**
  * Drag & drop
  */

void NoteEditorView::dropEvent(QDropEvent* event)
{
#if defined(__APPLE__) || defined(_WIN32)
    if(event->mimeData()->text().size())
    {
        MF_DEBUG("D&D drop event: '" << event->mimeData()->text().toStdString() << "'" << endl);
        signalDnDropUrl(event->mimeData()->text().replace("file:///",""));
    }
#else
    if(event->mimeData()->hasUrls()
         &&
       event->mimeData()->hasFormat("text/plain")
         &&
       event->mimeData()->urls().size())
    {
        MF_DEBUG("D&D drop: '" << event->mimeData()->urls().first().url().trimmed().toStdString() << "'" << endl);
        signalDnDropUrl(event->mimeData()->urls().first().url().replace("file://",""));
    }
#endif

    event->acceptProposedAction();
}

void NoteEditorView::dragMoveEvent(QDragMoveEvent* event)
{
    // needed to protect text cursor functionality after drop
    event->acceptProposedAction();
}

/*
 * Formatting
 */

void NoteEditorView::wrapSelectedText(const QString &tag, const QString &endTag)
{
    QTextCursor cursor = textCursor();
    QTextDocument *doc = document();
    int start = cursor.selectionStart();
    int end = cursor.selectionEnd();
    if(cursor.hasSelection() && doc->findBlock(start) == doc->findBlock(end)) {
        cursor.beginEditBlock();
        QString text = cursor.selectedText();
        text.prepend(tag);
        if(endTag.size()) text.append(endTag); else text.append(tag);
        cursor.insertText(text);
        cursor.endEditBlock();
        cursor.setPosition(start + tag.length());
        cursor.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, end - start);
        setTextCursor(cursor);
    } else if(!cursor.hasSelection()) {
        if(endTag.size()) {
            cursor.insertText(tag+endTag);
            cursor.movePosition(QTextCursor::Left, QTextCursor::MoveAnchor, endTag.length());
        } else {
            cursor.insertText(tag+tag);
            cursor.movePosition(QTextCursor::Left, QTextCursor::MoveAnchor, tag.length());
        }
        setTextCursor(cursor);
    }

    setFocus();
}

void NoteEditorView::insertMarkdownText(const QString &text, bool newLine, int offset)
{
    QTextCursor cursor = textCursor();
    if(cursor.hasSelection()) {
        cursor.clearSelection();
    }
    if(newLine) {
        cursor.movePosition(QTextCursor::StartOfLine);
        cursor.movePosition(QTextCursor::Down);
    }
    cursor.insertText(text);
    cursor.movePosition(QTextCursor::Left, QTextCursor::MoveAnchor, text.length()-offset);
    setTextCursor(cursor);

    setFocus();
}

/*
 * Associations
 */

QString NoteEditorView::getRelevantWords() const
{
    // IMPROVE get whole line and cut word on which is curser and it before/after siblings: return textCursor().block().text(); ...
    QString result{};
    if(textCursor().block().text().size()) {
        QString t = textCursor().block().text();
        int c = textCursor().positionInBlock();
        if(t[c]!=' ') {
            // extend c to LEFT and to RIGHT
            for(int i=c-1; i>=0; i--) {
                if(t[i]==' ') break;
                result.prepend(t[i]);
            }
            for(int i=c; i<t.size(); i++) {
                if(t[i]==' ') break;
                result += t[i];
            }
        }
    }
    return result;
    //return textCursor().block().text();
}

/*
 * Autocomplete
 */

void NoteEditorView::keyPressEvent(QKeyEvent* event)
{
    hitCounter++;

    // TODO Linux paste

    if(event->modifiers() & Qt::ControlModifier) {
        switch (event->key()) {
        case Qt::Key_V: {
            // TODO make this private function
            QClipboard* clip = QApplication::clipboard();
            const QMimeData* mime = clip->mimeData();
            if(mime->hasImage()) {
                MF_DEBUG("Image PASTED to editor" << endl);
                QImage image = qvariant_cast<QImage>(mime->imageData());
                emit signalPasteImageData(image);
                return;
            }
            break;
        }
        case Qt::Key_F: {
            findStringAgain();
            return; // exit to override default key binding
        }
        }
    }

    // IMPROVE get configuration reference and editor mode setting - this must be fast
    if(Configuration::getInstance().getEditorKeyBinding()==Configuration::EditorKeyBindingMode::EMACS) {
        if(event->modifiers() & Qt::ControlModifier){
            switch (event->key()) {
            case Qt::Key_A:
                moveCursor(QTextCursor::StartOfLine);
                return; // exit to override default key binding
            }
        }
    }

    if(completedAndSelected && handledCompletedAndSelected(event)) {
        return;
    } else {
        completedAndSelected = false;
    }

    if(completer->popup()->isVisible()) {
        switch(event->key()) {
            case Qt::Key_Up:
            case Qt::Key_Down:
            case Qt::Key_Enter:
            case Qt::Key_Return:
            case Qt::Key_Escape:
                event->ignore();
                return;
            default:
                completer->popup()->hide();
                break;
        }
    } else {
        switch(event->key()) {
            case Qt::Key_Tab:
            if(tabsAsSpaces) {
                insertTab();
                return;
            }
            break;
        }
    }

    QPlainTextEdit::keyPressEvent(event);

    // completion: letter must be handled~inserted first - now it's time to autocomplete
    if(Configuration::getInstance().isUiEditorEnableAutocomplete()) {
        if(!completer->popup()->isVisible()) {
            if(blockCount() < Configuration::EDITOR_MAX_AUTOCOMPLETE_LINES) {
                QChar k{event->key()};
                if(k.isLetter()) {
                    if(performTextCompletion()) {
                        event->ignore();
                    }
                }
            }
        }
    }
}

void NoteEditorView::mousePressEvent(QMouseEvent* event)
{
    if(completedAndSelected) {
        completedAndSelected = false;
        QTextCursor cursor = textCursor();
        cursor.removeSelectedText();
        setTextCursor(cursor);
    }
    QPlainTextEdit::mousePressEvent(event);
}

bool NoteEditorView::handledCompletedAndSelected(QKeyEvent *event)
{
    completedAndSelected = false;

    QTextCursor cursor = textCursor();
    switch(event->key()) {
    case Qt::Key_Space:
        case Qt::Key_Enter:
        case Qt::Key_Return:
            cursor.clearSelection();
            break;
        case Qt::Key_Escape:
            cursor.removeSelectedText();
            break;
        default:
            return false;
    }

    setTextCursor(cursor);
    event->accept();
    return true;
}

const QString NoteEditorView::getCompletionPrefix()
{
    QTextCursor cursor = textCursor();
    cursor.select(QTextCursor::WordUnderCursor);
    const QString completionPrefix = cursor.selectedText();
    if(!completionPrefix.isEmpty() && completionPrefix.at(completionPrefix.length()-1).isLetter()) {
        return completionPrefix;
    } else {
        return QString{};
    }
}

bool NoteEditorView::performTextCompletion()
{
    const QString completionPrefix = getCompletionPrefix();
    if(!completionPrefix.isEmpty()) {
        performTextCompletion(completionPrefix);
        return true;
    }

    return false;
}

void NoteEditorView::performTextCompletion(const QString& completionPrefix)
{
    MF_DEBUG("Completing prefix: '" << completionPrefix.toStdString() << "'" << endl);

    // TODO model population is SLOW, don't do it after each hit, but e.g. when user does NOT write
    populateModel(completionPrefix);

    completer->setCompletionMode(QCompleter::PopupCompletion);
    if(completionPrefix != completer->completionPrefix()) {
        completer->setCompletionPrefix(completionPrefix);
        completer->popup()->setCurrentIndex(completer->completionModel()->index(0, 0));
    }

    // do NOT complete inline - it completes what user doesn't know and is bothering
    //if(completer->completionCount() == 1) {
    //    insertCompletion(completer->currentCompletion(), true);
    //} else {
        QRect rect = cursorRect();
        rect.setWidth(
            completer->popup()->sizeHintForColumn(0) +
            completer->popup()->verticalScrollBar()->sizeHint().width());
        completer->complete(rect);
    //}
}

void NoteEditorView::slotStartLinkCompletion()
{
    const QString completionPrefix = getCompletionPrefix();
    if(!completionPrefix.isEmpty()) {
        // ask mind (via Orloj) for links > editor gets signal whose handlings opens completion dialog
        emit signalGetLinksForPattern(completionPrefix);
    }
}

void NoteEditorView::slotPerformLinkCompletion(
    const QString& completionPrefix,
    vector<string>* links)
{
    MF_DEBUG("Completing prefix: '" << completionPrefix.toStdString() << "' w/ " << links->size() << " links" << endl);

    if(!links->empty()) {
        // populate model for links
        QStringList linksAsStrings{};
        for(string& s:*links) {
            linksAsStrings.append(QString::fromStdString(s));
        }
        // IMPROVE sort links so that they are the most relevant
        model->setStringList(linksAsStrings);
        delete links;

        // perform completion
        int COMPLETER_POPUP_WIDTH=fontMetrics().averageCharWidth()*50;
        completer->setCompletionMode(QCompleter::UnfilteredPopupCompletion);
        completer->popup()->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
        completer->popup()->setMaximumWidth(COMPLETER_POPUP_WIDTH);
        completer->popup()->setMinimumHeight(fontMetrics().height()*3);
        if(completionPrefix != completer->completionPrefix()) {
            completer->setCompletionPrefix(completionPrefix);
            completer->popup()->setCurrentIndex(completer->completionModel()->index(0, 0));
        }
        QRect rect = cursorRect();
        rect.setWidth(COMPLETER_POPUP_WIDTH);
        completer->complete(rect);
    }
}

void NoteEditorView::populateModel(const QString& completionPrefix)
{
    QStringList strings = toPlainText().split(QRegExp{"\\W+"});
    strings.removeAll(completionPrefix);
    strings.removeDuplicates();
    qSort(strings.begin(), strings.end(), caseInsensitiveLessThan);
    model->setStringList(strings);
}

void NoteEditorView::insertCompletion(const QString& completion, bool singleWord)
{
    QTextCursor cursor = textCursor();

    int insertionPosition;
    if(completion.startsWith("[")) {
        for(int i=0; i<completer->completionPrefix().length(); i++) {
            cursor.deletePreviousChar();
        }
        // single word completion to be removed (not used, but migth be useful)
        insertionPosition = cursor.position();
        cursor.insertText(completion.right(completion.length()));
    } else {
        int numberOfCharsToComplete
            = completion.length() - completer->completionPrefix().length();
        // single word completion to be removed (not used, but migth be useful)
        insertionPosition = cursor.position();
        cursor.insertText(completion.right(numberOfCharsToComplete));
    }

    if(singleWord) {
        cursor.setPosition(insertionPosition);
        cursor.movePosition(QTextCursor::EndOfWord, QTextCursor::KeepAnchor);
        completedAndSelected = true;
    }
    setTextCursor(cursor);
}

void NoteEditorView::insertTab()
{
    QString completion{};
    if(tabWidth == 8) {
        completion.append("        ");
    } else {
        completion.append("    ");
    }
    QTextCursor cursor = textCursor();
    cursor.insertText(completion);
}

/*
 * L&F
 */

void NoteEditorView::highlightCurrentLine()
{
    QList<QTextEdit::ExtraSelection> extraSelections;
    if(!isReadOnly()) {
        QTextEdit::ExtraSelection selection;
        QBrush highlightColor = palette().alternateBase();
        selection.format.setBackground(highlightColor);
        selection.format.setProperty(QTextFormat::FullWidthSelection, true);
        selection.cursor = textCursor();
        selection.cursor.clearSelection();
        extraSelections += selection;

        if(isVisible()) {
            QString m{"  ("};
            m += QString::number(textCursor().blockNumber());
            m += ":";
            m += QString::number(textCursor().positionInBlock());
            m += ")";
            statusBar->showInfo(m);
        }
    }
    setExtraSelections(extraSelections);
}

/*
 * Line number panel
 */

int NoteEditorView::lineNumberPanelWidth()
{
    if(showLineNumbers) {
        int digits = 1;
        int max = qMax(1, blockCount());
        while(max >= 10) {
            max /= 10;
            ++digits;
        }
        int space = 3 + fontMetrics().width(QLatin1Char{'9'}) * digits;
        return space;
    } else {
        return 0;
    }
}

void NoteEditorView::updateLineNumberPanelWidth(int newBlockCount)
{
    UNUSED_ARG(newBlockCount);

    // IMPROVE comment to parameter and ignore macro
    setViewportMargins(lineNumberPanelWidth(), 0, 0, 0);
}

void NoteEditorView::updateLineNumberPanel(const QRect& r, int deltaY)
{
    if(showLineNumbers) {
        if(deltaY) {
            lineNumberPanel->scroll(0, deltaY);
        } else {
            lineNumberPanel->update(0, r.y(), lineNumberPanel->width(), r.height());
        }

        if (r.contains(viewport()->rect())) {
            updateLineNumberPanelWidth(0);
        }
    }
}

void NoteEditorView::resizeEvent(QResizeEvent* e)
{
    QPlainTextEdit::resizeEvent(e);
    QRect contents = contentsRect();
    lineNumberPanel->setGeometry(QRect(contents.left(), contents.top(), lineNumberPanelWidth(), contents.height()));
}

void NoteEditorView::lineNumberPanelPaintEvent(QPaintEvent* event)
{
    QPainter painter(lineNumberPanel);
    if(!LookAndFeels::getInstance().isThemeNative()) {
        painter.fillRect(event->rect(), LookAndFeels::getInstance().getEditorLineNumbersBackgroundColor());
    }

    QTextBlock block = firstVisibleBlock();
    int blockNumber = block.blockNumber();
    int top = static_cast<int>(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + static_cast<int>(blockBoundingRect(block).height());

    int currentLine = textCursor().blockNumber();
    while(block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            QString number = QString::number(blockNumber + 1);
            if(blockNumber == currentLine) {
                painter.setPen(QString{"#AA0000"});
            } else {
                painter.setPen(LookAndFeels::getInstance().getEditorLineNumbersForegroundColor());
            }
            painter.drawText(0, top, lineNumberPanel->width(), fontMetrics().height(), Qt::AlignCenter, number);
        }

        block = block.next();
        top = bottom;
        bottom = top + static_cast<int>(blockBoundingRect(block).height());
        ++blockNumber;
    }
}

/*
 * Search
 */

void NoteEditorView::findString(const QString s, bool reverse, bool caseSensitive, bool wholeWords)
{
    lastFindString = s;
    lastFindReverse = reverse;
    lastCaseSensitive = caseSensitive;
    lastWholeWords = wholeWords;

    QTextDocument::FindFlags flag;
    if(reverse) flag |= QTextDocument::FindBackward;
    if(caseSensitive) flag |= QTextDocument::FindCaseSensitively;
    if(wholeWords) flag |= QTextDocument::FindWholeWords;

    QTextCursor cursor = this->textCursor();
    QTextCursor cursorSaved = cursor;

    if(!find(s, flag)) {
        // nothing is found > jump to start/end
        cursor.movePosition(reverse?QTextCursor::End:QTextCursor::Start);

        // the cursor is set at the beginning/end of the document (if search is reverse or not),
        // in the next "find", if the word is found, now you will change the cursor position
        setTextCursor(cursor);

        if(!find(s, flag)) {
            QMessageBox::information(
                        this,
                        tr("Full-text Search Result"),
                        tr("No matching text found."),
                        QMessageBox::Ok,
                        QMessageBox::Ok);

            // set the cursor back to its initial position
            setTextCursor(cursorSaved);
        }
    }
}

void NoteEditorView::findStringAgain()
{
    if(!lastFindString.isEmpty()) {
        findString(lastFindString, lastFindReverse, lastCaseSensitive, lastWholeWords);
    }
}

} // m8r namespace
