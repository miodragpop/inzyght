# Inzyght Homepage

This directory contains the web interface for the Inzyght Ycash blockchain explorer.

## File Structure

```
public/
├── index.html          # Main homepage
├── css/
│   └── style.css       # Custom Bootstrap styles
├── js/
│   └── main.js         # Main JavaScript functionality
└── README.md           # This file
```

## Features

### Homepage Components

1. **Navigation Bar**
   - Logo with Inzyght branding
   - Quick navigation links (Home, Blocks, Transactions, Addresses, API)
   - Responsive mobile menu

2. **Hero Section**
   - Eye-catching header with branding
   - Clear description of the explorer's purpose

3. **Search Bar**
   - Search for blocks, transactions, or addresses
   - Supports multiple input formats:
     - Block height (numeric)
     - Block/Transaction hash (64-character hex)
     - Address (20-100 character string)

4. **Network Statistics**
   - Current block height
   - Total transactions
   - Network difficulty
   - Active peer connections
   - Real-time updates every 30 seconds

5. **Latest Blocks Table**
   - Recent blocks with details:
     - Block height
     - Block hash (truncated with link)
     - Timestamp
     - Transaction count
     - Mining pool/miner
     - Block size
     - Block reward

6. **Latest Transactions Table**
   - Recent transactions with details:
     - Transaction hash (truncated with link)
     - From address (truncated with link)
     - To address (truncated with link)
     - Amount transferred
     - Transaction fee
     - Confirmation status

7. **Features Showcase**
   - Six feature cards highlighting:
     - Block Explorer
     - Transaction Tracking
     - Address Search
     - Network Statistics
     - REST API
     - Security & Performance

8. **Network Information Section**
   - About Inzyght
   - Current network statistics
   - Real-time data display

9. **Footer**
   - Quick links
   - Community resources
   - Copyright information

## Technologies Used

- **Bootstrap 5.3** - Responsive CSS framework
- **jQuery 3.6** - DOM manipulation and AJAX
- **Font Awesome 4.7** - Icons
- **Custom CSS** - Themed styling for Ycash colors

## Color Scheme

- **Primary (Success):** #198754 (Green) - Ycash brand color
- **Secondary (Info):** #0dcaf0 (Cyan)
- **Dark:** #212529 - Text and backgrounds
- **Light:** #f8f9fa - Light backgrounds

## JavaScript Functions

### Utility Functions

- `formatHash(hash, length)` - Truncate hashes with ellipsis
- `formatNumber(num)` - Format numbers with thousand separators
- `formatDate(timestamp)` - Format UNIX timestamp to readable date
- `formatBalance(balance)` - Format YEC balances
- `formatDifficulty(difficulty)` - Format difficulty with M/K suffixes

### Data Functions

- `fetchNetworkInfo()` - Fetch network statistics
- `fetchLatestBlocks()` - Fetch recent blocks
- `fetchLatestTransactions()` - Fetch recent transactions
- `updateStatistics(data)` - Update stats cards
- `displayLatestBlocks(blocks)` - Render blocks table
- `displayLatestTransactions(transactions)` - Render transactions table

### Interactive Functions

- `setupSearch()` - Initialize search functionality
- `performSearch()` - Execute search based on query type

## API Integration Points

The homepage is designed to work with the following API endpoints (to be implemented):

```
GET /api/v1/network/info
GET /api/v1/blocks/latest
GET /api/v1/transactions/latest
GET /api/v1/search?q={query}
GET /api/v1/block/{height|hash}
GET /api/v1/tx/{hash}
GET /api/v1/address/{address}
```

## Current Status

The homepage currently displays **mock data** for demonstration purposes. To enable real data:

1. Implement the API endpoints listed above
2. Update the fetch functions in `js/main.js` to call actual endpoints
3. Replace mock data with API responses

## Responsive Design

The homepage is fully responsive and works on:
- Desktop (1920px+)
- Tablet (768px-1024px)
- Mobile (< 768px)

All tables and components adapt to smaller screens with appropriate sizing and touch-friendly interactions.

## Customization

### Changing Colors

Edit the CSS variables in `css/style.css`:

```css
:root {
    --primary-color: #0d6efd;
    --success-color: #198754;
    --danger-color: #dc3545;
    --warning-color: #ffc107;
    --info-color: #0dcaf0;
    --dark-color: #212529;
    --light-color: #f8f9fa;
}
```

### Modifying Layouts

Edit `index.html` to customize:
- Navigation structure
- Section layout
- Card arrangements
- Table columns

### Adjusting Styles

Edit `css/style.css` to modify:
- Spacing and padding
- Fonts and sizes
- Colors and backgrounds
- Animations and transitions

## Browser Compatibility

- Chrome 90+
- Firefox 88+
- Safari 14+
- Edge 90+
- Mobile browsers (iOS Safari, Chrome Mobile)

## Performance

- Lightweight CSS framework (Bootstrap 5.3)
- Minimal JavaScript dependencies (jQuery)
- Responsive images and optimized assets
- Fast data refresh (30-second intervals)
- No blocking scripts

## Future Enhancements

Planned improvements:
- Real-time WebSocket updates for live data
- Advanced search filters
- Transaction graph visualization
- Block mining statistics
- Address balance tracking
- Export functionality (CSV, JSON)
- Dark mode toggle
- Multi-language support

## Support

For issues or feature requests regarding the homepage:
1. Check existing GitHub issues
2. Create a new issue with details
3. Include browser and device information

## License

Same as Inzyght project - See LICENSE file in project root
