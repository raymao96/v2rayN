namespace ServiceLib.Models.Entities;

public class RoutingItem
{
    [PrimaryKey]
    public string Id { get; set; }

    public string Remarks { get; set; }
    public string Url { get; set; }

    [Ignore, JsonIgnore]
    public string DisplayRemarks => string.IsNullOrEmpty(Url) ? Remarks switch
    {
        "V4-繞過大陸(Whitelist)" or "V4-绕过大陆(Whitelist)" => ResUI.RoutingPresetWhitelist,
        "V4-黑名單(Blacklist)" or "V4-黑名单(Blacklist)" => ResUI.RoutingPresetBlacklist,
        "V4-全域(Global)" or "V4-全局(Global)" => ResUI.RoutingPresetGlobal,
        _ => Remarks,
    } : Remarks;
    public string RuleSet { get; set; }
    public int RuleNum { get; set; }
    public bool Enabled { get; set; } = true;
    public bool Locked { get; set; }
    public string CustomIcon { get; set; }
    public string CustomRulesetPath4Singbox { get; set; }
    public string DomainStrategy { get; set; }
    public string DomainStrategy4Singbox { get; set; }
    public int Sort { get; set; }
    public bool IsActive { get; set; }
}
