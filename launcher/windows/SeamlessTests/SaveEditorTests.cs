using System.Text.Json;
using BloodborneLauncher.Core;

internal static class SaveEditorTests
{
    internal static void Run()
    {
        var slot = SaveEditor.Parse<SaveEditor.Slot[]>("""
            [{"slot":2,"name":"测试","level":25,"echoes":2147483647}]
            """)[0];
        if (slot.SlotNumber != 2 || slot.Name != "测试" || slot.Echoes != int.MaxValue)
            throw new Exception("Save slot JSON contract changed");
        var data = SaveEditor.Parse<SaveEditor.Snapshot>("""
            {"revision":"abc","character":{"name":"Test","stats":{"vigor":10},"level":25,
             "echoes":500,"insight":6,"ng_cycle":2},"items":[
             {"section":0,"slot":1,"kind":"goods","number":100,"name":null,"count":5,"level":null},
             {"section":0,"slot":0,"kind":"weapon","number":22000300,"name":"Test","count":1,"level":3}]}
            """);
        if (data.Character.NgCycle != 2 || data.Character.Stats["vigor"] != 10 ||
            data.Items[0].Level != null || data.Items[1].Level != 3)
            throw new Exception("Save character or item JSON contract changed");
        if (data.Flags != null || data.Findings != null || data.Items[1].MaxLevel != null)
            throw new Exception("Older edit data without progress fields must still parse");
        var progress = SaveEditor.Parse<SaveEditor.Snapshot>("""
            {"revision":"abc","character":{"name":"Test","stats":{"vigor":10},"level":25,"echoes":1,"insight":0,"ng_cycle":0},
             "items":[{"section":0,"slot":0,"kind":"weapon","number":22000300,"name":"Test","count":1,"level":3,"max_level":10}],
             "flags":[{"id":12101800,"kind":"boss","group":"Central Yharnam","label":"Cleric Beast [12101800]","on":true}],
             "findings":{"phantom":true,"duplicate_keys":["Bell"],"world_empty":false}}
            """);
        if (progress.Items[0].MaxLevel != 10 || !progress.Flags![0].On || progress.Flags[0].Kind != "boss" ||
            !progress.Findings!.Phantom || progress.Findings.DuplicateKeys[0] != "Bell")
            throw new Exception("Save progress and findings JSON contract changed");
        var progressEdit = JsonSerializer.Serialize(new { Flags = new[] { new { Id = 12101800, On = false } },
            Repair = new[] { "phantom" } }, SaveEditor.Json);
        if (!progressEdit.Contains("\"id\":12101800") || !progressEdit.Contains("\"repair\":[\"phantom\"]"))
            throw new Exception("Save progress edit JSON contract changed");
        var payload = JsonSerializer.Serialize(new { data.Revision, NgCycle = 3,
            Items = new[] { new { Section = 0, Slot = 1, Count = 6 } } }, SaveEditor.Json);
        using var edit = JsonDocument.Parse(payload);
        if (edit.RootElement.GetProperty("ng_cycle").GetInt32() != 3 ||
            edit.RootElement.GetProperty("items")[0].GetProperty("count").GetInt32() != 6)
            throw new Exception("Save edit JSON contract changed");
        if (SaveEditor.Parse<SaveEditor.WriteResult>("{\"backup\":null}").Backup != null ||
            SaveEditor.Parse<SaveEditor.Backup[]>("[{\"name\":\"backup\",\"path\":\"folder\",\"reason\":\"before-edit\"}]")[0].Name != "backup")
            throw new Exception("Save backup JSON contract changed");
        Console.WriteLine("PASS: save editor JSON parsing and edit serialization");
    }
}
