/*
 mind.cpp     MindForger thinking notebook

 Copyright (C) 2016-2022 Martin Dvorak <martin.dvorak@mindforger.com>

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
#include "mind.h"

#ifdef MF_MD_2_HTML_CMARK
  #include "ai/autolinking/autolinking_mind.h"
  #include "ai/autolinking/cmark_aho_corasick_block_autolinking_preprocessor.h"
#else
  #include "ai/autolinking/naive_autolinking_preprocessor.h"
#endif

using namespace std;

namespace m8r {

Mind::Mind(Configuration &configuration)
    : config{configuration},
      ontology{},
#if defined  MF_MD_2_HTML_CMARK
      autoInterceptor(new CmarkAhoCorasickBlockAutolinkingPreprocessor{*this}),
#else
      autoInterceptor(new NaiveAutolinkingPreprocessor{*this}),
#endif
      htmlRepresentation{ontology, autoInterceptor},
      mdConfigRepresentation(new MarkdownConfigurationRepresentation{}),
      memory{configuration, ontology, htmlRepresentation},
#ifdef MF_MD_2_HTML_CMARK
      autolinking{new AutolinkingMind{*this}},
#else
      autolinking{nullptr},
#endif
      exclusiveMind{},
      timeScopeAspect{},
      tagsScopeAspect{ontology},
      scopeAspect{timeScopeAspect, tagsScopeAspect}
{
    ai = new Ai{memory,*this};
    deleteWatermark = 0;
    activeProcesses = 0;
    associationsSemaphore = 0;

    knowledgeGraph = new KnowledgeGraph{this};

    timeScopeAspect.setTimeScope(config.getTimeScope());
    tagsScopeAspect.setTags(config.getTagsScope());
    memory.setMindScope(&scopeAspect);

    stats = new MindStatistics();
    stats->mostReadOutline = nullptr;
    stats->mostWrittenOutline = nullptr;
    stats->mostReadNote = nullptr;
    stats->mostWrittenNote = nullptr;
    stats->mostUsedTag = nullptr;
}

Mind::~Mind()
{
    delete ai;
    delete knowledgeGraph;
    delete mdConfigRepresentation;
    delete autoInterceptor;
    delete autolinking;
    delete stats;

    // - Memory destruct outlines
    // - allNotesCache Notes is just container referencing Memory's Outlines
}

/*
 * THINKING
 */

bool Mind::learn()
{
    MF_DEBUG("@Learn" << endl);
    lock_guard<mutex> criticalSection{exclusiveMind};

    if(config.getMindState()!=Configuration::MindState::DREAMING && !activeProcesses) {
        MF_DEBUG("Learning..." << endl);
        mindAmnesia();
        memory.learn();
#ifdef MF_MD_2_HTML_CMARK
        autolinking->reindex();
#endif
        MF_DEBUG("Mind LEARNED " << memory.getOutlinesCount() << " Os" << endl);
        return true;
    } else {
        MF_DEBUG("Learn: CANNOT learn because Mind is DREAMING and/or there are " << activeProcesses << " active Mind processes" << endl);
        return false;
    }
}

shared_future<bool> Mind::think()
{
    MF_DEBUG("@Think w/ threshold " << config.getAsyncMindThreshold() << endl);
    lock_guard<mutex> criticalSection{exclusiveMind};

    if(config.getMindState()==Configuration::MindState::SLEEPING) {
        if(config.getAsyncMindThreshold() > memory.getNotesCount()) {
            // get ready for thinking - dream() changes state to THINKING on its finish
            return mindDream();
        } else {
            // IMPROVE design ASYNC AI/AA to handle also huge repositories
            MF_DEBUG("Think: CANNOT think because number of Notes in Mind is too big" << endl);
            persistMindState(Configuration::MindState::SLEEPING);
            promise<bool> p;
            p.set_value(false);
            return p.get_future();
        }
    } else {
        MF_DEBUG("Think: CANNOT think because Mind is DREAMING or already THINKING (asleep first)" << endl);
        promise<bool> p;
        p.set_value(false);
        return p.get_future();
    }
}

/* It does NOT need mutex because it's private and can be called from Mind only.
 * This method may run long time. It ALWAYS switches mind state to THINKING when finishes.
 */
shared_future<bool> Mind::mindDream()
{
    MF_DEBUG("@Dream" << endl);

    // Mind is expected to be clean and SLEEPING
    if(config.getMindState()==Configuration::MindState::SLEEPING) {
        MF_DEBUG("Dreaming..." << endl);
        config.setMindState(Configuration::MindState::DREAMING);

        // sanity
        // o integrity check: ...
        // o memory structure check:
        //  - Os w/o description
        //  - Os w/o any N
        //  - Ns w/o description
        // o links
        //  - broken links (target doesn't exist)
        //  - orphan files/images not referenced from any O or N by a link

        // triples: infer all triples, check, fix, optimize and save

        // AI: AA, NN, ... may take long time to finish
        return ai->dream();
    } else {
        MF_DEBUG("Dream: CANNOT dream because Mind is not ready ~ SLEEPING (asleep first)" << endl);
        promise<bool> p;
        p.set_value(false);
        return p.get_future();
    }
}

bool Mind::sleep()
{
    MF_DEBUG("@Sleep" << endl);
    lock_guard<mutex> criticalSection{exclusiveMind};
    if(mindSleep()) {
        persistMindState(Configuration::MindState::SLEEPING);
        return true;
    } else {
        return false;
    }
}

/*
 *  This method does NOT need mutex because it's private and it's called from Mind only
 */
bool Mind::mindSleep()
{
    if(config.getMindState()!=Configuration::MindState::DREAMING && !activeProcesses) {
        // AI can asleep ONLY if there are no active mental processes
        if(ai->sleep()) {
            meditateAssociations();

            allNotesCache.clear();
            memoryDwell.clear();
            triples.clear();

            MF_DEBUG("Mind IS sleeping..." << endl);
            return true;
        } else {
            MF_DEBUG("Sleep: CANNOT asleep because there are " << activeProcesses << " active Mind processes" << endl);
            return false;
        }
    } else {
        MF_DEBUG("Sleep: CANNOT asleep because Mind is DREAMING (wait for " << activeProcesses << " dreaming processes to finish)" << endl);
        // DREAMING cannot be cancelled > wait for dream() to finish before calling sleep() again.
        return false;
    }
}

bool Mind::amnesia()
{
    MF_DEBUG("@Amnesia" << endl);
    lock_guard<mutex> criticalSection{exclusiveMind};
    if(mindAmnesia()) {
        persistMindState(Configuration::MindState::SLEEPING);
        return true;
    } else {
        return false;
    }
}

shared_future<bool> Mind::getAssociatedNotes(AssociatedNotes& associations)
{
    lock_guard<mutex> criticalSection{exclusiveMind};

    if(config.getMindState()==Configuration::MindState::THINKING) {
        switch(associations.getSourceType()) {
        case OUTLINE:
            MF_DEBUG("Outline associations..." << endl);
            return ai->getAssociatedNotes(associations.getOutline(), *associations.getAssociations());
            break;
        case NOTE:
            MF_DEBUG("Note associations..." << endl);
            return ai->getAssociatedNotes(associations.getNote(), *associations.getAssociations());
        case WORD:
            MF_DEBUG("Word associations..." << endl);
            return ai->getAssociatedNotes(associations.getWord(), *associations.getAssociations(), associations.getNote());
        default:
            ; // NOP
        }
    }

    associations.getAssociations()->clear();
    promise<bool> p{};
    p.set_value(false);
    return shared_future<bool>(p.get_future());
}

/*
 *  This method does NOT need mutex because it's private and it's called from Mind only
 */
bool Mind::mindAmnesia()
{
    if(config.getMindState()!=Configuration::MindState::DREAMING && !activeProcesses) {
        mindSleep();

        // forget EVERYTHING
        memory.amnesia();
#ifdef MF_MD_2_HTML_CMARK
        autolinking->clear();
#endif
        MF_DEBUG("Mind WITH amnesia" << endl);
        return true;
    } else {
        MF_DEBUG("Amnesia: CANNOT asleep because Mind is DREAMING (wait for " << activeProcesses << " dreaming processes to finish)" << endl);
        return false;
    }
}

/*
 * Autolinking
 */

void Mind::autolinkUpdate(const std::string& oldName, const std::string& newName) const
{
#ifdef MF_MD_2_HTML_CMARK
    autolinking->update(oldName, newName);
#endif
}

bool Mind::autolinkFindLongestPrefixWord(std::string& s, std::string& r) const
{
#ifdef MF_MD_2_HTML_CMARK
    return autolinking->findLongestPrefixWord(s, r);
#else
    return false;
#endif
}

/*
 * Remembering
 */


void Mind::remember(const std::string& outlineKey)
{
    memory.remember(outlineKey);

    // TODO onRemembering()

#ifdef MF_MD_2_HTML_CMARK
    if(config.isAutolinking()) {
        autolinking->reindex();
    }
#endif
}

void Mind::remember(Outline* outline)
{
    memory.remember(outline);

#ifdef MF_MD_2_HTML_CMARK
    if(config.isAutolinking()) {
        autolinking->reindex();
    }
#endif
}

void Mind::forget(Outline* outline)
{
    memory.forget(outline);

    // TODO onRemembering()

#ifdef MF_MD_2_HTML_CMARK
    if(config.isAutolinking()) {
        autolinking->reindex();
    }
#endif
}


const vector<Note*>& Mind::getMemoryDwell(int pageSize) const
{
    UNUSED_ARG(pageSize);

    return memoryDwell;
}

size_t Mind::getMemoryDwellDepth() const
{
    return memoryDwell.size();
}

/*
vector<Note*>* Mind::findNoteByNameFts(const string& pattern) const
{
    UNUSED_ARG(pattern);

    return nullptr;
}
*/

void Mind::getOutlineNames(vector<string>& names) const
{
    // IMPROVE PERF cache vector (stack member) until and evict on memory modification
    vector<Outline*> outlines = memory.getOutlines();
    for(Outline* outline:outlines) {
        names.push_back(outline->getName());
    }
}

// One match in either title or body is enought to be added to the result
void Mind::findNoteFts(
        vector<Note*>* result,
        const string& pattern,
        const FtsSearch searchMode,
        Outline* outline)
{
    // IMPROVE make this faster - do NOT convert to lower case, but compare it in that method > will do less
    // IMPROVE avoid duplicate code - introduce an pre-processing iface (lower/nop) and used one code
    if(searchMode == FtsSearch::IGNORE_CASE) {
        string s{};
        stringToLower(outline->getName(), s);
        if(s.find(pattern)!=string::npos) {
            result->push_back(outline->getOutlineDescriptorAsNote());
        } else {
            for(string* d:outline->getDescription()) {
                if(d) {
                    s.clear();
                    stringToLower(*d, s);
                    if(s.find(pattern)!=string::npos) {
                        result->push_back(outline->getOutlineDescriptorAsNote());
                        break;
                    }
                }
            }
        }
        for(Note* note:outline->getNotes()) {
            if(scopeAspect.isOutOfScope(note)) {
                continue;
            }
            s.clear();
            stringToLower(note->getName(), s);
            if(s.find(pattern)!=string::npos) {
                result->push_back(note);
            } else {
                for(string* d:note->getDescription()) {
                    if(d) {
                        s.clear();
                        stringToLower(*d, s);
                        if(s.find(pattern)!=string::npos) {
                            result->push_back(note);
                            break;
                        }
                    }
                }
            }
        }
    } else if (searchMode == FtsSearch::EXACT) {
        if(outline->getName().find(pattern)!=string::npos) {
            result->push_back(outline->getOutlineDescriptorAsNote());
        } else {
            for(string* d:outline->getDescription()) {
                if(d && d->find(pattern)!=string::npos) {
                    result->push_back(outline->getOutlineDescriptorAsNote());
                    // avoid multiple matches in the result
                    break;
                }
            }
        }
        for(Note* note:outline->getNotes()) {
            if(scopeAspect.isOutOfScope(note)) {
                continue;
            }
            if(note->getName().find(pattern)!=string::npos) {
                result->push_back(note);
            } else {
                for(string* d:note->getDescription()) {
                    if(d && d->find(pattern)!=string::npos) {
                        result->push_back(note);
                        // avoid multiple matches in the result
                        break;
                    }
                }
            }
        }
    } else if (searchMode == FtsSearch::REGEXP) {
        std::smatch matchedString;
        std::regex regex{pattern};
        if(std::regex_search(outline->getName(), matchedString, regex)) {
            result->push_back(outline->getOutlineDescriptorAsNote());
        } else {
            for(string* d:outline->getDescription()) {
                if(d && std::regex_search(*d, matchedString, regex)) {
                    result->push_back(outline->getOutlineDescriptorAsNote());
                    // avoid multiple matches in the result
                    break;
                }
            }
        }
        for(Note* note:outline->getNotes()) {
            if(scopeAspect.isOutOfScope(note)) {
                continue;
            }
            if(std::regex_search(outline->getName(), matchedString, regex)) {
                result->push_back(note);
            } else {
                for(string* d:note->getDescription()) {
                    if(d && std::regex_search(*d, matchedString, regex)) {
                        result->push_back(note);
                        // avoid multiple matches in the result
                        break;
                    }
                }
            }
        }
    }
}

// IMPROVE consider result be parameter passed by caller (reuse, mem)
vector<Note*>* Mind::findNoteFts(const string& pattern, FtsSearch searchMode, Outline* outlineScope)
{
    if(allNotesCache.size()) {
        allNotesCache.clear();
    }

    vector<Note*>* result = new vector<Note*>();

    string r{};
    if(searchMode == FtsSearch::IGNORE_CASE) {
        stringToLower(pattern, r);
    } else {
        r.assign(pattern);
    }

    if(outlineScope) {
        findNoteFts(result, r, searchMode, outlineScope);
    } else {
        const vector<m8r::Outline*> outlines = memory.getOutlines();
        for(Outline* outline:outlines) {
            if(scopeAspect.isOutOfScope(outline)) {
                continue;
            }
            findNoteFts(result, r, searchMode, outline);
        }
    }
    return result;
}

vector<Note*>* Mind::getReferencedNotes(const Note& note) const
{
    UNUSED_ARG(note);

    return nullptr;
}

vector<Note*>* Mind::getReferencedNotes(const Note& note, const Outline& outline) const
{
    UNUSED_ARG(note);
    UNUSED_ARG(outline);

    return nullptr;
}

vector<Note*>* Mind::getRefereeNotes(const Note& note) const
{
    UNUSED_ARG(note);

    return nullptr;
}

vector<Note*>* Mind::getRefereeNotes(const Note& note, const Outline& outline) const
{
    UNUSED_ARG(note);
    UNUSED_ARG(outline);

    return nullptr;
}

void Mind::findNotesByTags(const vector<const Tag*>& tags, vector<Note*>& result) const
{
    vector<Note*> allNotes{};
    memory.getAllNotes(allNotes);
    for(Note* n:allNotes) {
        const vector<const Tag*>* thingTags = n->getTags();
        bool hasAllTags=true;
        for(size_t i=0; i<tags.size(); i++) {
            if(std::find(
                        thingTags->begin(),
                        thingTags->end(),
                        tags.at(i)) == thingTags->end())
            {
                hasAllTags=false;
                break;
            }
        }
        if(hasAllTags) {
            result.push_back(n);
        }
    }
}

void Mind::getAllThings(
    vector<Thing*>& things,
    vector<string>* thingsNames,
    string* pattern,
    ThingNameSerialization as,
    Outline* currentO)
{
    const vector<Outline*>& os = getOutlines();
    for(Outline* o:os) {
        if((pattern && stringStartsWith(o->getName(), *pattern))
              ||
            pattern==nullptr)
        {
            things.push_back(o);
            if(thingsNames) {
                string s{};
                switch(as) {
                case ThingNameSerialization::LINK:
                    // IMPROVE make this Note's method
                    {
                        s += "[";
                        s += o->getName();
                        s += "](";
                        string p = RepositoryIndexer::makePathRelative(
                             config.getActiveRepository(),
                             currentO?currentO->getKey():o->getKey(),
                             o->getKey());
                        pathToLinuxDelimiters(p, p);
                        s += p;
                        s += ")";
                        break;
                    }
                case ThingNameSerialization::NAME:
                case ThingNameSerialization::SCOPED_NAME:
                default:
                    s += o->getName();
                    break;
                }
                thingsNames->push_back(s);
            }
        }
    }
    vector<Note*> ns{};
    getAllNotes(ns);
    for(Note* n:ns) {
        if((pattern && stringStartsWith(n->getName(), *pattern))
              ||
            pattern==nullptr)
        {
            things.push_back(n);
            if(thingsNames) {
                string s{};
                switch(as) {
                case ThingNameSerialization::NAME:
                    s += n->getName();
                    break;
                case ThingNameSerialization::LINK:
                    // IMPROVE make this Note's method
                    {
                        s += "[";
                        s += n->getName();
                        s += " (";
                        s += n->getOutline()->getName();
                        s += ")](";
                        string p = RepositoryIndexer::makePathRelative(
                             config.getActiveRepository(),
                             currentO?currentO->getKey():n->getOutline()->getKey(),
                             n->getKey());
                        pathToLinuxDelimiters(p, p);
                        s += p;
                        s += ")";
                        break;
                    }
                case ThingNameSerialization::SCOPED_NAME:
                default:
                    {
                        // IMPROVE make this Note's method: getScopedName()
                        s += n->getName();
                        s += " (";
                        s += n->getOutline()->getName();
                        s += ")";
                        break;
                    }
                }
                thingsNames->push_back(s);
            }
        }
    }
}

const vector<Outline*>& Mind::getOutlines() const
{
    // IMPROVE PERF use dirty flag to avoid result-rebuilt
    static vector<Outline*> result{};

    if(scopeAspect.isEnabled()) {
        result.clear();
        for(Outline* o:memory.getOutlines()) {
            if(scopeAspect.isInScope(o)) {
                result.push_back(o);
            }
        }
        return result;
    } else {
        return memory.getOutlines();
    }
}

vector<Outline*>* Mind::getOutlinesOfType(const OutlineType& type) const
{
    UNUSED_ARG(type);

    return nullptr;
}

std::vector<Note*>& Mind::getAllNotes(vector<Note*>& notes, bool sortByRead, bool addNoteForOutline) const
{
    return memory.getAllNotes(notes, sortByRead, addNoteForOutline);
}

vector<Note*>* Mind::getNotesOfType(const NoteType& type) const
{
    UNUSED_ARG(type);

    return nullptr;
}

vector<Note*>* Mind::getNotesOfType(const NoteType& type, const Outline& outline) const
{
    UNUSED_ARG(type);
    UNUSED_ARG(outline);

    return nullptr;
}

void Mind::findOutlinesByTags(const std::vector<const Tag*>& tags, std::vector<Outline*>& result) const
{
    for(Outline* o:memory.getOutlines()) {
        bool allMatched = true;
        for(size_t i=0; i<tags.size(); i++) {
            if(std::find(
                        o->getTags()->begin(),
                        o->getTags()->end(),
                        tags.at(i)) == o->getTags()->end())
            {
                allMatched = false;
                break;
            }
        }
        if(allMatched) {
            result.push_back(o);
        }
    }
}

vector<Tag*>* Mind::getOutlinesTags() const
{
    return nullptr;
}

Taxonomy<Tag>& Mind::getTags()
{
    return ontology.getTags();
}

void Mind::getTagsCardinality(map<const Tag*,int>& tagsCardinality)
{
    if(ontology.getTags().size()) {
        for(const Tag* t:ontology.getTags().values()) {
            // IMPROVE make NONE exclusion faster (checks in three loops below)
            if(!stringistring(string("none"), t->getName())) {
                tagsCardinality[t] = 0;
            }
        }
        const vector<Outline*>& outlines = memory.getOutlines();
        bool doO, doN;
        for(Outline* o:outlines) {
            doO = false;
            if(scopeAspect.isEnabled()) {
                if(scopeAspect.isInScope(o)) {
                    doO = true;
                }
            } else {
                doO = true;
            }
            if(doO) {
                for(const Tag* ot:*o->getTags()) {
                    if(!stringistring(string("none"), ot->getName())) {
                        tagsCardinality[ot] = tagsCardinality[ot]+1;
                    }
                }

                doN = false;
                for(Note* n:o->getNotes()) {
                    if(scopeAspect.isEnabled()) {
                        if(scopeAspect.isInScope(n)) {
                            doN = true;
                        }
                    } else {
                        doN = true;
                    }
                    if(doN) {
                        for(const Tag* nt:*n->getTags()) {
                            if(!stringistring(string("none"), nt->getName())) {
                                tagsCardinality[nt] = tagsCardinality[nt]+1;
                            }
                        }
                    }
                }
            }
        }
    } else {
        tagsCardinality.clear();
    }
}

vector<Tag*>* Mind::getNoteTags(const Outline& outline) const
{
    UNUSED_ARG(outline);

    return nullptr;
}

unsigned Mind::getTagCardinality(const Tag& tag) const
{
    UNUSED_ARG(tag);

    return 0;
}

unsigned Mind::getOutlineTagCardinality(const Tag& tag) const
{
    UNUSED_ARG(tag);

    return 0;
}

unsigned Mind::getNoteTagCardinality(const Tag& tag) const
{
    UNUSED_ARG(tag);

    return 0;
}

void Mind::removeTagFromOutlines(const Tag* tag, vector<Outline*>& modifiedOutlines)
{
    vector<const Tag*> tags{};
    tags.push_back(tag);
    for(Outline* o:memory.getOutlines()) {
        if(o->removeTag(tag)) {
            modifiedOutlines.push_back(o);
        }
    }
}

bool Mind::setOutlineUniqueTag(const Tag* tag, const string& outlineKey)
{
    Outline* o=memory.getOutline(outlineKey);
    if(o) {
        // strip home tag from all other Outlines
        vector<Outline*> modifiedOutlines{};
        removeTagFromOutlines(tag, modifiedOutlines);
        for(Outline* mo:modifiedOutlines) {
            // persist Os w/ removed T (timestamp not changed)
            memory.remember(mo->getKey());
        }

        // mark O as modified
        o->addTag(tag);
        memory.remember(o->getKey());
        return true;
    } else {
        return false;
    }
}

string Mind::outlineNew(
    // IMPROVE pass name by reference
    const string* name,
    const OutlineType* outlineType,
    const int8_t importance,
    const int8_t urgency,
    const int8_t progress,
    const vector<const Tag*>* tags,
    const vector<string*>* preamble,
    Stencil* outlineStencil)
{
    string key = memory.createOutlineKey(name);
    Outline* outline{};
    if(outlineStencil) {
        outline = memory.createOutline(outlineStencil);
        outline->setModified();
    } else {
        outline = new Outline{ontology.getDefaultOutlineType()};
    }

    if(preamble && preamble->size()) {
        outline->setPreamble(*preamble);
    }

    if(outline) {
        outline->completeProperties(datetimeNow());
        outline->setKey(key);
        if(name && !name->empty()) {
            outline->setName(*name);
        }
        if(outlineType) {
            outline->setType(outlineType);
        }
        outline->setImportance(importance);
        outline->setUrgency(urgency);
        outline->setProgress(progress);
        if(tags) {
            for(const Tag* t:*tags) {
                outline->addTag(t);
            }
        }
        if(outline->getNotes().empty()) {
            Note* note = new Note{memory.getOntology().getNoteTypes().get(NoteType::KeyNote()), outline};
            note->completeProperties(datetimeNow());
            outline->addNote(note);
        }

        remember(outline);
        onRemembering();
    } else {
        throw MindForgerException("Unable to create new Outline!");
    }

    return outline?outline->getKey():nullptr;
}

string Mind::outlineNew(Outline* outline)
{
    if(outline) {
        remember(outline);
        onRemembering();
    }

    return outline?outline->getKey():nullptr;
}


Outline* Mind::learnOutlineTWiki(const string& twikiFile)
{
    string directory{}, file{};
    pathToDirectoryAndFile(twikiFile, directory, file);
    if(!file.size()) {
        file.assign("twiki-outline");
    }
    string outlineKey = memory.createOutlineKey(&file);
    if(memory.learnOutlineTWiki(twikiFile, outlineKey)) {
        // IMPROVE: this is heavy operation - load just O
        learn();
        Outline* o = memory.getOutline(outlineKey);
        if(o) {
            // add twiki and import tags
            o->addTag(memory.getOntology().findOrCreateTag("twiki"));
            o->addTag(memory.getOntology().findOrCreateTag("import"));

            remember(o);
            onRemembering();
            return o;
        } else {
            MF_DEBUG("Unable to load imported O: " << outlineKey << endl);
            return nullptr;
        }
    } else {
        return nullptr;
    }
}


Outline* Mind::outlineClone(const std::string& outlineKey)
{
    Outline* o = memory.getOutline(outlineKey);
    if(o) {
        Outline* clonedOutline = new Outline{*o};
        clonedOutline->setKey(memory.createOutlineKey(&o->getName()));
        memory.remember(clonedOutline);
        onRemembering();
        return clonedOutline;
    } else {
        return nullptr;
    }
}

bool Mind::outlineForget(string outlineKey)
{
    Outline* o = memory.getOutline(outlineKey);
    if(o) {
        deleteWatermark++;

        forget(o);
        auto k = memory.createLimboKey(&o->getName());
        o->setKey(k);
        moveFile(outlineKey, k);
        return true;
    }
    return false;
}

Note* Mind::noteNew(
        const std::string& outlineKey,
        const uint16_t offset,
        // IMPROVE pass name by reference
        const std::string* name,
        const NoteType* noteType,
        u_int16_t depth,
        const std::vector<const Tag*>* tags,
        const int8_t progress,
        Stencil* noteStencil)
{
    Outline* o = memory.getOutline(outlineKey);
    if(o) {
        Note* n = memory.createNote(noteStencil);
        if(!n) {
            // IMPROVE make note type method parameter w/ a default
            n = new Note(ontology.findOrCreateNoteType(NoteType::KeyNote()),o);
        }
        n->setOutline(o);
        if(name) {
            n->setName(*name);
        }
        n->setModified();
        if(noteType) {
            n->setType(noteType);
        }
        n->setDepth(depth);
        if(tags) {
            n->setTags(tags);
        }
        n->setProgress(progress);
        n->completeProperties(n->getModified());

        n->setReadPretty();
        n->setModifiedPretty();

        o->addNote(n, NO_PARENT==offset?0:offset);
        return n;
    } else {
        throw MindForgerException("Outline for given key not found!");
    }
}

Note* Mind::noteClone(const string& outlineKey, const Note* newNote, const bool deep)
{
    Outline* o = memory.getOutline(outlineKey);
    if(o) {
        return o->cloneNote(newNote, deep);
    } else {
        throw MindForgerException("Outline for given key not found!");
    }
}

Outline* Mind::noteRefactor(Note* noteToRefactor, const string& targetOutlineKey, Note* targetParent)
{
    UNUSED_ARG(targetParent);

    if(noteToRefactor) {
        Outline* targetOutline = memory.getOutline(targetOutlineKey);
        if(targetOutline) {
            vector<Note*> children{};
            Outline* sourceOutline = noteToRefactor->getOutline();
            sourceOutline->getAllNoteChildren(noteToRefactor, &children);
            children.insert(children.begin(), noteToRefactor);
            // IMPROVE allow passing parent for the Note in the target Outline
            targetOutline->addNotes(children, 0);

            sourceOutline->removeNote(noteToRefactor);

            memory.remember(sourceOutline);
            memory.remember(targetOutline);

            return targetOutline;
        } else {
            throw MindForgerException("Outline for given key not found!");
        }
    } else {
        throw MindForgerException("Note to be refactored is nullptr!");
    }
}

Outline* Mind::noteForget(Note* note)
{
    Outline* o = note->getOutline();
    if(o) {
        deleteWatermark++;

        note->getOutline()->forgetNote(note);
        return o;
    } else {
        throw MindForgerException("Unable find Outline from which should be the Note deleted!");
    }
}

void Mind::noteUp(Note* note, Outline::Patch* patch)
{
    if(note) {
        note->getOutline()->moveNoteUp(note, patch);
    }
}

void Mind::noteDown(Note* note, Outline::Patch* patch)
{
    if(note) {
        note->getOutline()->moveNoteDown(note, patch);
    }
}

void Mind::noteFirst(Note* note, Outline::Patch* patch)
{
    if(note) {
        note->getOutline()->moveNoteToFirst(note, patch);
    }
}

void Mind::noteLast(Note* note, Outline::Patch* patch)
{
    if(note) {
        note->getOutline()->moveNoteToLast(note, patch);
    }
}

void Mind::notePromote(Note* note, Outline::Patch* patch)
{
    if(note) {
        note->getOutline()->promoteNote(note, patch);
    }
}

void Mind::noteDemote(Note* note, Outline::Patch* patch)
{
    if(note) {
        note->getOutline()->demoteNote(note, patch);
    }
}

void Mind::noteOnRename(const std::string& oldName, const std::string& newName)
{
#ifdef MF_MD_2_HTML_CMARK
    autolinking->update(oldName, newName);
#endif
}

void Mind::onRemembering()
{
    allNotesCache.clear();
}

MindStatistics* Mind::getStatistics()
{
    // IMPROVE cache it until memory is dirty
    const vector<Outline*>&os = memory.getOutlines();
    if(os.size()) {
        u_int32_t maxReads=0;
        u_int32_t maxWrites=0;
        for(Outline* o:os) {
            if(o->getReads() > maxReads) {
                maxReads = o->getReads();
                stats->mostReadOutline = o;
            }
            if(o->getRevision() > maxWrites) {
                maxWrites = o->getRevision();
                stats->mostWrittenOutline = o;
            }
        }
    } else {
        stats->mostReadOutline = nullptr;
        stats->mostWrittenOutline = nullptr;
    }

    vector<Note*> ns{};
    memory.getAllNotes(ns);
    if(ns.size()) {
        u_int32_t maxReads=0;
        u_int32_t maxWrites=0;
        for(Note* n:ns) {
            if(n->getReads() > maxReads) {
                maxReads = n->getReads();
                stats->mostReadNote = n;
            }
            if(n->getRevision() > maxWrites) {
                maxWrites = n->getRevision();
                stats->mostWrittenNote = n;
            }
        }
    } else {
        stats->mostReadNote = nullptr;
        stats->mostWrittenNote = nullptr;
    }

    map<const Tag*,int> ts{};
    getTagsCardinality(ts);
    if(ts.size()) {
        map<const Tag*,int>::iterator it{};
        int maxCardinality = 0;
        for(it = ts.begin(); it != ts.end(); it++) {
            if(it->second > maxCardinality) {
                stats->mostUsedTag = it->first;
                maxCardinality = it->second;
            }
        }
    } else {
        stats->mostUsedTag = nullptr;
    }

    return stats;
}

/*
 * NER
 */

#ifdef MF_NER

bool Mind::isNerInitilized() const
{
    return ai->isNerInitialized();
}

void Mind::recognizePersons(const Outline* outline, int entityFilter, std::vector<NerNamedEntity>& result) {
    ai->recognizePersons(outline, entityFilter, result);
}

#endif

// unique_ptr template BREAKS Qt Developer indentation > stored at EOF
unique_ptr<vector<Outline*>> Mind::findOutlineByNameFts(const string& pattern) const
{
    // IMPROVE implement regexp and other search options by reusing HSTR code
    // IMPROVE PERF this method is extremely inefficient > use cached map (stack member) evicted on memory modification
    unique_ptr<vector<Outline*>> result{new vector<Outline*>()};
    if(pattern.size()) {
        vector<Outline*> outlines = memory.getOutlines();
        for(Outline* outline:outlines) {
            if(!pattern.compare(outline->getName())) {
                result->push_back(outline);
            }
        }
    }
    return result;
}

} /* namespace */
