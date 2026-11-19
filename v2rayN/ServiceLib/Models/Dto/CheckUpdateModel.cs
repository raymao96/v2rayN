namespace ServiceLib.Models.Dto;

public partial class CheckUpdateModel : ReactiveObject
{
    public const string GeoFiles = "GeoFiles";

    public bool? IsSelected { get; set; }
    public bool IsCheckPreRelease { get; set; }
    public bool ShowCheckPreRelease { get; init; }
    public ECoreType? CoreType { get; init; }
    [Reactive] public partial string? Remarks { get; set; }
    public string CoreTypeForStorage => CoreType?.ToString() ?? GeoFiles;
}
