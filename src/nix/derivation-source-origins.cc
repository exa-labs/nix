// `nix derivation source-origins`: map the `inputSrcs` of derivations
// back to the filesystem locations they were copied from during
// evaluation. This is the missing link between `nix derivation show`
// (which only knows about store paths) and the working-tree paths
// that produced them.

#include "nix/cmd/command.hh"
#include "nix/cmd/common-eval-args.hh"
#include "nix/main/common-args.hh"
#include "nix/store/store-api.hh"
#include "nix/store/derivations.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-settings.hh"

#include <nlohmann/json.hpp>

#include <deque>

using json = nlohmann::json;

namespace nix {

struct CmdDerivationSourceOrigins : InstallablesCommand, MixPrintJSON
{
    bool recursive = true;

    CmdDerivationSourceOrigins()
    {
        addFlag({
            .longName = "no-recursive",
            .description = "Only show the `inputSrcs` of the specified derivations, not those of their transitive "
                           "`inputDrvs`.",
            .handler = {&recursive, false},
        });
    }

    std::string description() override
    {
        return "map the input sources of a derivation back to their original filesystem paths";
    }

    std::string doc() override
    {
        return
#include "derivation-source-origins.md"
            ;
    }

    Category category() override
    {
        return catUtility;
    }

    /**
     * Resolve the source path that `inputSrc` was copied from to a
     * filesystem path. Returns `std::nullopt` if the store path wasn't
     * produced by this evaluation.
     */
    static std::optional<std::string> resolveSourcePath(EvalState & state, const StorePath & inputSrc)
    {
        /* Best case: `recordPathOrigin()` already resolved the store
           path to an original filesystem location (flake inputs,
           `builtins.path`, `cleanSourceWith`, ...). */
        if (auto origPath = state.getOriginalPath(inputSrc))
            return origPath->string();

        /* Otherwise fall back to the raw source path, if any. */
        if (auto srcPath = state.getSourceOrigin(inputSrc)) {
            if (auto physical = srcPath->getPhysicalPath())
                return physical->string();
            return srcPath->to_string();
        }

        return std::nullopt;
    }

    /**
     * Enumerate the files in the store object `inputSrc` and map each
     * one back to a path under `sourceRoot`. Since the store object is
     * the *result* of any filtering (e.g. `cleanSourceWith`), this
     * gives file-level precision even when `sourceRoot` is a broad
     * directory. Returns an empty array if `inputSrc` is not a
     * directory or isn't available in the store.
     */
    static json
    enumerateSourceFiles(Store & store, const StorePath & inputSrc, const std::filesystem::path & sourceRoot)
    {
        json files = json::array();

        auto accessor = store.getFSAccessor(inputSrc);
        if (!accessor)
            return files;

        auto rootStat = accessor->maybeLstat(CanonPath::root);
        if (!rootStat || rootStat->type != SourceAccessor::tDirectory)
            return files;

        /* Breadth-first traversal. Symlinks are treated as leaf entries
           rather than followed, both to avoid cycles (`link -> .`) and to
           avoid enumerating files outside the store object. */
        std::deque<CanonPath> dirs{CanonPath::root};
        while (!dirs.empty()) {
            auto dir = std::move(dirs.front());
            dirs.pop_front();
            for (auto & [name, entryType] : accessor->readDirectory(dir)) {
                auto entry = dir / name;
                auto type = entryType ? *entryType : accessor->lstat(entry).type;
                if (type == SourceAccessor::tDirectory)
                    dirs.push_back(std::move(entry));
                else
                    files.push_back((sourceRoot / std::string(entry.rel())).string());
            }
        }

        return files;
    }

    void run(ref<Store> store, Installables && installables) override
    {
        /* The evaluation cache short-circuits evaluation of derivation
           attributes, in which case the paths are never copied to the
           store during this evaluation and no provenance is recorded. */
        evalSettings.useEvalCache = false;

        auto state = getEvalState();

        /* Evaluating the installables is what populates the provenance
           maps in `EvalState`. */
        auto drvPaths = Installable::toDerivations(store, installables, true);

        json jsonRoot = json::object();

        std::deque<StorePath> queue(drvPaths.begin(), drvPaths.end());
        StorePathSet seen;

        while (!queue.empty()) {
            auto drvPath = std::move(queue.front());
            queue.pop_front();

            if (!drvPath.isDerivation() || !seen.insert(drvPath).second)
                continue;

            auto drv = store->readDerivation(drvPath);

            json inputSrcsJson = json::object();

            for (auto & input : drv.inputs) {
                std::visit(
                    overloaded{
                        [&](const SingleDerivedPath::Opaque & inputSrc) {
                            json entry = json::object();
                            entry["storePath"] = store->printStorePath(inputSrc.path);

                            if (auto sourcePath = resolveSourcePath(*state, inputSrc.path)) {
                                entry["sourcePath"] = *sourcePath;
                                auto sourceFiles = enumerateSourceFiles(*store, inputSrc.path, *sourcePath);
                                if (!sourceFiles.empty())
                                    entry["sourceFiles"] = std::move(sourceFiles);
                            } else
                                entry["sourcePath"] = nullptr;

                            inputSrcsJson[store->printStorePath(inputSrc.path)] = std::move(entry);
                        },
                        [&](const SingleDerivedPath::Built & inputDrv) {
                            if (recursive)
                                queue.push_back(inputDrv.getBaseStorePath());
                        },
                    },
                    input.raw());
            }

            json drvJson = json::object();
            drvJson["drvPath"] = store->printStorePath(drvPath);
            drvJson["name"] = drv.name;
            drvJson["inputSrcs"] = std::move(inputSrcsJson);
            jsonRoot[store->printStorePath(drvPath)] = std::move(drvJson);
        }

        printJSON(jsonRoot);
    }
};

static auto rCmdDerivationSourceOrigins =
    registerCommand2<CmdDerivationSourceOrigins>({"derivation", "source-origins"});

} // namespace nix
