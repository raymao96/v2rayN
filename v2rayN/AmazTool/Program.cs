namespace AmazTool;

internal static class Program
{
    [STAThread]
    private static void Main(string[] args)
    {
        try
        {
            // If no arguments are provided, display usage guidelines and exit
            if (args.Length == 0)
            {
                ShowHelp();
                return;
            }

            // Log all arguments for debugging purposes
            foreach (var arg in args)
            {
                Console.WriteLine(arg);
            }

            // Parse command based on first argument
            switch (args[0].ToLowerInvariant())
            {
                case "rebootas":
                    // Handle application restart
                    HandleRebootAsync();
                    break;

                case "help":
                case "--help":
                case "-h":
                case "/?":
                    // Display help information
                    ShowHelp();
                    break;

                default:
                    ShowHelp();
                    break;
            }
        }
        catch (Exception ex)
        {
            // Global exception handling
            Console.WriteLine($"An error occurred: {ex.Message}");
            Console.WriteLine("Press any key to exit...");
            Console.ReadKey();
        }
    }

    /// <summary>
    /// Display help information and usage guidelines
    /// </summary>
    private static void ShowHelp()
    {
        Console.WriteLine(Resx.Resource.Guidelines);
        Console.WriteLine("Available commands:");
        Console.WriteLine("  rebootas             - Restart the application");
        Console.WriteLine("  help                 - Display this help information");
        Thread.Sleep(5000);
    }

    /// <summary>
    /// Handle application restart
    /// </summary>
    private static void HandleRebootAsync()
    {
        Console.WriteLine("Restarting application...");
        Thread.Sleep(1000);
        Utils.StartV2RayN();
    }
}
