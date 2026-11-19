using System.Formats.Tar;
using System.IO.Compression;

namespace ServiceLib.ViewModels;

public partial class CheckUpdateViewModel : MyReactiveObject
{
    public EventChannel<RxVoid> ReloadRequested { get; } = new();

    public BulkObservableCollection<CheckUpdateModel> CheckUpdateModels { get; } = [];
    public ReactiveCommand<RxVoid, RxVoid> CheckUpdateCmd { get; }
    public ReactiveCommand<RxVoid, RxVoid> CheckOnlyCmd { get; }

    [Reactive] public partial bool EnableUpdateViaProxy { get; set; }
    [Reactive] public partial bool IsBusy { get; private set; }

    public CheckUpdateViewModel()
    {
        _config = ConfigTextHandler.Config;
        var settings = _config.CheckUpdateItem;
        EnableUpdateViaProxy = settings.UpdateViaProxy;

        var manager = CoreInfoManager.Instance;
        var models = manager.GetCheckUpdateCoreTypes()
            .Where(type => type is ECoreType.Xray or ECoreType.mihomo or ECoreType.sing_box)
            .Select(type => new CheckUpdateModel
            {
                CoreType = type,
                IsSelected = settings.SelectedCoreTypes?.Contains(type.ToString()) ?? true,
                IsCheckPreRelease = settings.CheckPreReleaseCoreTypes?.Contains(type.ToString()) ?? false,
                ShowCheckPreRelease = manager.GetCheckPreRelease(type, true),
                Remarks = ResUI.menuCheckUpdate,
            })
            .ToList();
        models.Add(new CheckUpdateModel
        {
            IsSelected = settings.SelectedCoreTypes?.Contains(CheckUpdateModel.GeoFiles) ?? true,
            Remarks = ResUI.menuCheckUpdate,
        });
        CheckUpdateModels.ReplaceRange(models);

        var canRun = this.WhenAnyValue(vm => vm.IsBusy, busy => !busy);
        CheckOnlyCmd = ReactiveCommand.CreateFromTask(() => RunUpdates(false), canRun);
        CheckUpdateCmd = ReactiveCommand.CreateFromTask(() => RunUpdates(true), canRun);
        CheckOnlyCmd.ThrownExceptions.Subscribe(ReportError);
        CheckUpdateCmd.ThrownExceptions.Subscribe(ReportError);
    }

    private async Task RunUpdates(bool install)
    {
        if (IsBusy)
        {
            return;
        }

        IsBusy = true;
        var coreStopped = false;
        try
        {
            var selected = CheckUpdateModels.Where(item => item.IsSelected == true)
                .Select(item => (Item: item, PreRelease: item.ShowCheckPreRelease && item.IsCheckPreRelease))
                .ToArray();
            var useProxy = EnableUpdateViaProxy;
            var settings = _config.CheckUpdateItem;
            settings.SelectedCoreTypes = selected.Select(entry => entry.Item.CoreTypeForStorage).ToList();
            settings.CheckPreReleaseCoreTypes = CheckUpdateModels
                .Where(item => item.ShowCheckPreRelease && item.IsCheckPreRelease)
                .Select(item => item.CoreTypeForStorage)
                .ToList();
            settings.UpdateViaProxy = useProxy;
            await ConfigTextHandler.SaveConfig();

            // Finish network requests before stopping the proxy core.
            var packages = new List<(CheckUpdateModel Item, string Executable, string Destination, string Directory)>();
            try
            {
                foreach (var (item, preRelease) in selected)
                {
                    try
                    {
                        if (item.CoreType is not { } type)
                        {
                            await UpdateGeo(item, install, useProxy);
                            continue;
                        }

                        SetStatus(item, "...");
                        using var download = await DownloadService.CreateDownloadServiceWithCheck(useProxy);
                        var updater = new UpdateService(download, _config, (_, message) =>
                        {
                            SetStatus(item, message);
                            return Task.CompletedTask;
                        });
                        var result = await updater.CheckHasUpdateOnly(type, preRelease);
                        if (!result.Success || result.Version is null || string.IsNullOrEmpty(result.Url))
                        {
                            SetStatus(item, string.IsNullOrEmpty(result.Msg) ? ResUI.OperationFailed : result.Msg);
                            continue;
                        }

                        if (!install)
                        {
                            SetStatus(item, string.Format(ResUI.MsgCheckUpdateHasNewVersion, type, result.Version));
                            continue;
                        }

                        var package = await StageCore(type, result.Url, download, item);
                        packages.Add((item, package.Executable, package.Destination, package.Directory));
                    }
                    catch (Exception ex)
                    {
                        Logging.SaveLog(nameof(CheckUpdateViewModel), ex);
                        SetStatus(item, ex.Message);
                    }
                }

                if (packages.Count == 0)
                {
                    return;
                }

                coreStopped = true;
                await CoreManager.Instance.CoreStop();
                foreach (var (item, executable, destination, _) in packages)
                {
                    try
                    {
                        File.Move(executable, destination, overwrite: true);
                        SetStatus(item, ResUI.MsgUpdateV2rayCoreSuccessfully);
                    }
                    catch (Exception ex)
                    {
                        Logging.SaveLog(nameof(CheckUpdateViewModel), ex);
                        SetStatus(item, ex.Message);
                    }
                }
            }
            finally
            {
                foreach (var package in packages)
                {
                    CleanupPackage(package.Directory);
                }
            }
        }
        finally
        {
            if (coreStopped)
            {
                ReloadRequested.Publish();
            }
            IsBusy = false;
        }
    }

    private async Task UpdateGeo(CheckUpdateModel item, bool install, bool useProxy)
    {
        if (!install)
        {
            SetStatus(item, $"{ResUI.menuCheckOnly} ({ResUI.MsgNotSupport})");
            return;
        }

        using var download = await DownloadService.CreateDownloadServiceWithCheck(useProxy);
        Exception? failure = null;
        download.Error += (_, args) => failure = args.GetException();
        await new UpdateService(download, _config, (_, message) =>
        {
            SetStatus(item, message);
            return Task.CompletedTask;
        }).UpdateGeoFileAll();
        if (failure is not null)
        {
            throw failure;
        }
    }

    private async Task<(string Executable, string Destination, string Directory)> StageCore(
        ECoreType type, string url, DownloadService download, CheckUpdateModel item)
    {
        var manager = CoreInfoManager.Instance;
        var info = manager.GetCoreInfo(type);
        var executableNames = info?.CoreExes?.Select(Utils.GetExeName).ToArray() ?? [];
        var installPath = Utils.GetBinPath("", type.ToString());
        var stage = Path.Combine(installPath, $".update-{Guid.NewGuid():N}");
        Directory.CreateDirectory(stage);
        try
        {
            var urlPath = new Uri(url).AbsolutePath;
            var suffix = urlPath.EndsWith(".tar.gz", StringComparison.OrdinalIgnoreCase)
                ? ".tar.gz"
                : Path.GetExtension(urlPath).ToLowerInvariant();
            var archive = Path.Combine(stage, $"package{suffix}");
            var payload = Directory.CreateDirectory(Path.Combine(stage, "payload")).FullName;
            Exception? failure = null;
            download.Error += (_, args) => failure = args.GetException();
            download.UpdateCompleted += (_, args) => SetStatus(item, args.Msg);
            await download.DownloadFileAsync(new FileDownloadRequest { FileUrl = url, FilePath = archive });
            if (failure is not null)
            {
                throw new IOException(failure.Message, failure);
            }

            switch (suffix)
            {
                case ".zip" when FileUtils.ZipExtractToFile(archive, payload, "geo"):
                    break;
                case ".tar.gz":
                    await using (var gzip = new GZipStream(File.OpenRead(archive), CompressionMode.Decompress))
                    {
                        TarFile.ExtractToDirectory(gzip, payload, overwriteFiles: false);
                    }
                    break;
                case ".gz" when executableNames.Length > 0:
                    await using (var gzip = new GZipStream(File.OpenRead(archive), CompressionMode.Decompress))
                    await using (var binary = File.Create(Path.Combine(payload, executableNames[0])))
                    {
                        await gzip.CopyToAsync(binary);
                    }
                    break;
                default:
                    throw new InvalidDataException(ResUI.OperationFailed);
            }

            var files = Directory.GetFiles(payload, "*", SearchOption.AllDirectories);
            var executable = executableNames.Select(name => files.FirstOrDefault(
                    file => string.Equals(Path.GetFileName(file), name, StringComparison.OrdinalIgnoreCase)))
                .FirstOrDefault(File.Exists)
                ?? throw new InvalidDataException(ResUI.OperationFailed);
            if (Utils.IsNonWindows())
            {
                await Utils.SetLinuxChmod(executable);
            }
            var version = await Utils.GetCliWrapOutput(executable, info?.VersionArg);
            if (string.IsNullOrWhiteSpace(version) || !version.Contains(info?.Match ?? type.ToString(), StringComparison.OrdinalIgnoreCase))
            {
                throw new InvalidDataException(ResUI.OperationFailed);
            }

            var existing = manager.GetCoreExecFile(info, out _);
            var destination = string.IsNullOrEmpty(existing)
                ? Path.Combine(installPath, Path.GetFileName(executable))
                : existing;
            return (executable, destination, stage);
        }
        catch
        {
            CleanupPackage(stage);
            throw;
        }
    }

    private static void CleanupPackage(string directory)
    {
        try
        {
            Directory.Delete(directory, recursive: true);
        }
        catch (Exception ex)
        {
            Logging.SaveLog(nameof(CheckUpdateViewModel), ex);
        }
    }

    private static void SetStatus(CheckUpdateModel item, string message)
    {
        RxSchedulers.MainThreadScheduler.Schedule(() => item.Remarks = message);
    }

    private static void ReportError(Exception ex)
    {
        Logging.SaveLog(nameof(CheckUpdateViewModel), ex);
        NoticeManager.Instance.SendMessageAndEnqueue(ex.Message);
    }
}
