/* Host tests for the .NET GUI's production Windows path/profile preflight. */
using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Reflection;

internal static class WinGuiNetProfileTest
{
    private static int failures;
    private static Type mainForm;
    private static MethodInfo validateName;
    private static MethodInfo preflightItems;
    private static Type createItem;

    private static void Check(bool condition, string label)
    {
        if (!condition)
        {
            Console.Error.WriteLine("FAIL: " + label);
            failures++;
        }
    }

    private static bool ThrowsIOException(Action action)
    {
        try { action(); }
        catch (TargetInvocationException ex)
        {
            return ex.InnerException is IOException;
        }
        catch (IOException) { return true; }
        return false;
    }

    private static void Valid(string name, bool directory, string label)
    {
        try { validateName.Invoke(null, new object[] { name, directory }); }
        catch (Exception ex) { Check(false, label + " rejected: " + ex); }
    }

    private static void Invalid(string name, bool directory, string label)
    {
        Check(ThrowsIOException(delegate {
            validateName.Invoke(null, new object[] { name, directory });
        }), label + " was accepted");
    }

    private static void Preflight(string root, string[][] items)
    {
        Type listType = typeof(List<>).MakeGenericType(createItem);
        IList list = (IList)Activator.CreateInstance(listType);
        FieldInfo pathField = createItem.GetField("path");
        FieldInfo directoryField = createItem.GetField("directory");
        foreach (string[] item in items)
        {
            object value = Activator.CreateInstance(createItem, true);
            pathField.SetValue(value, Path.Combine(root, item[0]));
            directoryField.SetValue(value, item[1] == "dir");
            list.Add(value);
        }
        preflightItems.Invoke(null, new object[] { list, root });
    }

    private static void PreflightInvalid(string root, string[][] items, string label)
    {
        Check(ThrowsIOException(delegate { Preflight(root, items); }),
              label + " was accepted");
    }

    public static int Main(string[] args)
    {
        if (args.Length != 1)
        {
            Console.Error.WriteLine("usage: win_gui_net_profile.exe BoltPack.exe");
            return 2;
        }

        Assembly app = Assembly.LoadFrom(Path.GetFullPath(args[0]));
        mainForm = app.GetType("BoltPack.MainForm", true);
        validateName = mainForm.GetMethod("ValidateWindowsArchiveName",
            BindingFlags.NonPublic | BindingFlags.Static);
        preflightItems = mainForm.GetMethod("PreflightCreateItems",
            BindingFlags.NonPublic | BindingFlags.Static);
        createItem = mainForm.GetNestedType("CreateItem",
            BindingFlags.NonPublic);
        Check(validateName != null && preflightItems != null && createItem != null,
              "production preflight methods/types are discoverable");
        if (failures != 0) return 1;

        Valid("plain/child.txt", false, "ordinary path");
        Valid("folder/", true, "directory trailing slash");
        Valid(new string('a', 255), false, "255-unit component");
        Invalid(new string('a', 256), false, "256-unit component");
        string path259 = new string('a', 128) + "/" + new string('b', 130);
        Valid(path259, false, "259-unit path");
        Invalid(path259 + "c", false, "260-unit path");
        Invalid("CON.txt", false, "reserved device name");
        Invalid("COM\u00b9.txt", false, "superscript device alias");
        Invalid("name.", false, "trailing dot");
        Invalid("name ", false, "trailing space");
        Invalid("bad:name", false, "forbidden colon");
        Invalid("../escape", false, "parent traversal");
        Invalid("a//b", false, "empty path component");
        Valid("emoji-\U0001f9ea.txt", false, "supplementary Unicode name");

        string root = Path.Combine(Path.GetTempPath(),
            "boltpack-dotnet-profile-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        try
        {
            Preflight(root, new[] {
                new[] { "Folder", "dir" },
                new[] { Path.Combine("Folder", "child.txt"), "file" }
            });
            PreflightInvalid(root, new[] {
                new[] { "Readme.txt", "file" },
                new[] { "README.TXT", "file" }
            }, "case-insensitive collision");
            PreflightInvalid(root, new[] {
                new[] { "caf\u00e9.txt", "file" },
                new[] { "cafe\u0301.txt", "file" }
            }, "NFC collision");
            PreflightInvalid(root, new[] {
                new[] { "node", "file" },
                new[] { Path.Combine("node", "child.txt"), "file" }
            }, "file then descendant conflict");
            PreflightInvalid(root, new[] {
                new[] { Path.Combine("node", "child.txt"), "file" },
                new[] { "node", "file" }
            }, "descendant then file conflict");
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine("Unexpected preflight exception: " + ex);
            failures++;
        }
        finally
        {
            Directory.Delete(root, true);
        }

        if (failures != 0) return 1;
        Console.WriteLine(".NET GUI Windows profile preflight checks passed");
        return 0;
    }
}
